/*
 * mmu_sandbox.c — UAOS x86_64 MMU Sandbox Initialization
 *
 * Configures 4-level paging tables to isolate the 4 GB Amiga guest address
 * space on the host x86_64 platform.  Maps the entire 4 GB window using
 * 2 MB huge pages for efficiency, with one critical exception:
 *
 *   0x00B00000 – 0x00DFFFFF  (Amiga CIA + custom chip registers)
 *       → PAGE_PRESENT cleared, PAGE_NO_CACHE set
 *       → Any access faults are forwarded to page_fault_handler.c
 *
 * All other pages are mapped present, writable, and user-accessible.
 *
 * The completed PML4 root table is loaded into CR3 to activate the sandbox.
 *
 * Build note: This file contains inline asm and is intended for a freestanding
 * (bare-metal) x86_64 kernel target only.  It will not link against libc.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Page table entry flag bits (x86_64 long-mode)
 * ----------------------------------------------------------------------- */

#define PAGE_PRESENT   (1ULL << 0)   /* P  — page is present               */
#define PAGE_WRITABLE  (1ULL << 1)   /* R/W — read-write                   */
#define PAGE_USER      (1ULL << 2)   /* U/S — user accessible              */
#define PAGE_NO_CACHE  (1ULL << 4)   /* PCD — page-level cache disable     */
#define PAGE_HUGE      (1ULL << 7)   /* PS  — 2 MB or 1 GB page            */

/* -----------------------------------------------------------------------
 * Address layout constants
 * ----------------------------------------------------------------------- */

#define PAGE_2MB_SIZE         (2ULL * 1024 * 1024)
#define AMIGA_ADDRESS_SPACE   (4ULL * 1024 * 1024 * 1024)  /* 4 GB         */
#define NUM_2MB_PAGES         (AMIGA_ADDRESS_SPACE / PAGE_2MB_SIZE) /* 2048 */

/* Amiga custom chip / CIA hardware register window                        */
#define CHIP_WINDOW_START     0x00B00000ULL
#define CHIP_WINDOW_END       0x00DFFFFFULL  /* inclusive upper bound       */

/* -----------------------------------------------------------------------
 * Page table structures — aligned to 4 KB (one table = 512 × 8-byte PDEs)
 *
 * For a 4 GB guest window mapped with 2 MB pages we need:
 *   1 × PML4  (entry 0 covers the first 512 GB)
 *   1 × PDPT  (entry 0 covers the first 1 GB, entry 1 covers 1–2 GB, etc.)
 *   4 × PD    (each covers 1 GB = 512 × 2 MB pages)
 * ----------------------------------------------------------------------- */

#define PDPT_ENTRIES   4   /* one per GB of the 4 GB guest window          */
#define PD_ENTRIES     512 /* 512 × 2 MB = 1 GB per PD                    */

typedef uint64_t pml4_t[512] __attribute__((aligned(4096)));
typedef uint64_t pdpt_t[512] __attribute__((aligned(4096)));
typedef uint64_t pd_t[512]   __attribute__((aligned(4096)));

static pml4_t uaos_pml4;
static pdpt_t uaos_pdpt;
static pd_t   uaos_pd[PDPT_ENTRIES];

/* -----------------------------------------------------------------------
 * Guest VA window — demand-paged reservation for the M68k bridge
 *
 * The UAE bridge needs a 4 GB guest physical address space, but the kernel
 * has no heap that could satisfy a contiguous 4 GB allocation.  Instead the
 * window is a pure VA reservation: PD entries under PDPT[16..19] are
 * installed non-present and each 2 MB page is backed on first touch when
 * the page fault handler calls UAOS_VM_GuestWindowFault().
 *
 * The window sits at 16 GB — inside the 0–512 GB coverage of PML4[0] but
 * outside the 0–4 GB identity map, so a guest offset can never alias a
 * host physical address that is already in use.
 *
 * Physical backing comes from a static pool in .guest_ram (zeroed by the
 * bootstrap and re-zeroed per page on commit).  The Musashi glue bounds
 * all g_ram accesses to the first 16 MB of the window (GUEST_RAM_SIZE);
 * the pool covers that plus headroom for thunk-layer accesses.
 * ----------------------------------------------------------------------- */

#define GUEST_WIN_VBASE      (16ULL * 1024 * 1024 * 1024)  /* 16 GB        */
#define GUEST_WIN_SIZE       (4ULL  * 1024 * 1024 * 1024)  /* 4 GB         */
#define GUEST_WIN_PDPT_INDEX (GUEST_WIN_VBASE >> 30)       /* 16           */
#define GUEST_WIN_PD_COUNT   (GUEST_WIN_SIZE / (512ULL * PAGE_2MB_SIZE))

#define GUEST_POOL_PAGES     16   /* 16 × 2 MB = 32 MB of backing store  */

static pd_t     uaos_guest_pd[GUEST_WIN_PD_COUNT];
static uint8_t  uaos_guest_backing[(GUEST_POOL_PAGES + 1) * PAGE_2MB_SIZE]
    __attribute__((section(".guest_ram"), aligned(4096)));
static uint64_t uaos_pool_base;         /* 2 MB-aligned base (VA == PA)   */
static uint32_t uaos_pool_committed;    /* pages committed so far         */
static int      uaos_guest_win_active;
static int      uaos_mmu_initialised;

/* -----------------------------------------------------------------------
 * UAOS_MMU_IsChipPage — returns 1 if the 2 MB page starting at phys_base
 * overlaps the custom chip / CIA hardware window.
 * ----------------------------------------------------------------------- */

static inline int UAOS_MMU_IsChipPage(uint64_t phys_base)
{
    uint64_t page_end = phys_base + PAGE_2MB_SIZE - 1;
    return (phys_base <= CHIP_WINDOW_END) && (page_end >= CHIP_WINDOW_START);
}

/* -----------------------------------------------------------------------
 * UAOS_MMU_Init — build and install the sandbox paging tables
 *
 * Call once at kernel startup before enabling paging or after a bare-metal
 * CR3 reload is safe.  Clobbers uaos_pml4, uaos_pdpt, and uaos_pd[].
 * ----------------------------------------------------------------------- */

void UAOS_MMU_Init(void)
{
    /* Zero all tables */
    for (int i = 0; i < 512; i++) uaos_pml4[i] = 0;
    for (int i = 0; i < 512; i++) uaos_pdpt[i] = 0;
    for (int g = 0; g < PDPT_ENTRIES; g++)
        for (int i = 0; i < 512; i++) uaos_pd[g][i] = 0;
    for (int g = 0; g < (int)GUEST_WIN_PD_COUNT; g++)
        for (int i = 0; i < 512; i++) uaos_guest_pd[g][i] = 0;

    /* Re-init tears down any previously reserved guest window */
    uaos_guest_win_active = 0;
    uaos_pool_committed   = 0;

    /* PML4[0] → PDPT (covers virtual addresses 0–512 GB)                  */
    uaos_pml4[0] = (uint64_t)(uintptr_t)uaos_pdpt
                   | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

    /* PDPT entries 0–3 → PD[0]–PD[3] (each covers 1 GB)                  */
    for (int g = 0; g < PDPT_ENTRIES; g++) {
        uaos_pdpt[g] = (uint64_t)(uintptr_t)uaos_pd[g]
                       | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }

    /* Populate 2 MB page descriptors across all 4 PDs                     */
    for (int g = 0; g < PDPT_ENTRIES; g++) {
        for (int i = 0; i < PD_ENTRIES; i++) {
            uint64_t phys = ((uint64_t)g << 30) | ((uint64_t)i << 21);

            if (UAOS_MMU_IsChipPage(phys)) {
                /* Custom chip / CIA window:
                 *   - PAGE_PRESENT cleared  → any access triggers #PF
                 *   - PAGE_NO_CACHE set      → cache-inhibit marker
                 *   - PAGE_HUGE set          → retains 2 MB page descriptor
                 *     format so the fault handler can identify the entry   */
                uaos_pd[g][i] = phys | PAGE_NO_CACHE | PAGE_HUGE;
            } else {
                /* Normal RAM page: present, writable, huge (2 MB)         */
                uaos_pd[g][i] = phys
                                 | PAGE_PRESENT
                                 | PAGE_WRITABLE
                                 | PAGE_USER
                                 | PAGE_HUGE;
            }
        }
    }

    /* Load PML4 base address into CR3 to activate the new page tables.
     * Bits 11:0 of CR3 are control flags; bit 3 = PWT, bit 4 = PCD.
     * We pass the raw physical address with no flags (write-back, cached). */
    uint64_t cr3_value = (uint64_t)(uintptr_t)uaos_pml4;
    __asm__ volatile (
        "mov %0, %%cr3"
        :
        : "r" (cr3_value)
        : "memory"
    );

    uaos_mmu_initialised = 1;
}

/* -----------------------------------------------------------------------
 * UAOS_MMU_GetPML4Base — returns the host pointer to the PML4 table,
 * useful for the page fault handler to remap chip-window entries at runtime.
 * ----------------------------------------------------------------------- */

uint64_t *UAOS_MMU_GetPML4Base(void)
{
    return (uint64_t *)uaos_pml4;
}

/* -----------------------------------------------------------------------
 * TLB shootdown helper — reload CR3 to drop all cached translations.
 * ----------------------------------------------------------------------- */

static void UAOS_VM_FlushTLB(void)
{
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

/* -----------------------------------------------------------------------
 * UAOS_VM_ReserveGuestWindow — reserve the 4 GB guest VA window
 *
 * Installs PDPT[16..19] -> uaos_guest_pd[] with every 2 MB entry
 * non-present, then arms the demand-paging path.  Returns the window base
 * on success, NULL if the sandbox page tables are not active yet.
 *
 * Must be called after UAOS_MMU_Init() (a re-init clears the window).
 * Idempotent: re-reserving resets the window and the backing pool.
 * ----------------------------------------------------------------------- */

void *UAOS_VM_ReserveGuestWindow(void)
{
    if (!uaos_mmu_initialised) return NULL;

    for (int g = 0; g < (int)GUEST_WIN_PD_COUNT; g++) {
        for (int i = 0; i < PD_ENTRIES; i++) uaos_guest_pd[g][i] = 0;
        uaos_pdpt[GUEST_WIN_PDPT_INDEX + g] =
            (uint64_t)(uintptr_t)uaos_guest_pd[g]
            | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }

    /* 2 MB-align the pool base so committed pages are legal huge frames */
    uaos_pool_base = ((uint64_t)(uintptr_t)uaos_guest_backing
                      + PAGE_2MB_SIZE - 1) & ~(PAGE_2MB_SIZE - 1);
    uaos_pool_committed   = 0;
    uaos_guest_win_active = 1;

    /* Drop stale translations for the window range (a previous window may
     * have left committed entries cached). */
    UAOS_VM_FlushTLB();

    return (void *)(uintptr_t)GUEST_WIN_VBASE;
}

/* -----------------------------------------------------------------------
 * UAOS_VM_GuestWindowFault — page fault hook for the guest window
 *
 * Called by the #PF handler for every fault.  If the fault address lies
 * inside the guest window and its 2 MB page is not yet committed, a backing
 * page is allocated from the pool, zeroed, mapped present, and the stale
 * translation invalidated.
 *
 * Returns:
 *    1  fault handled — the faulting instruction can be re-executed
 *    0  fault is outside the window (or hits a committed page) — not ours
 *   -1  inside the window but the backing pool is exhausted
 * ----------------------------------------------------------------------- */

int UAOS_VM_GuestWindowFault(uint64_t fault_addr)
{
    if (!uaos_guest_win_active) return 0;
    if (fault_addr < GUEST_WIN_VBASE ||
        fault_addr >= GUEST_WIN_VBASE + GUEST_WIN_SIZE) return 0;

    uint64_t off = fault_addr - GUEST_WIN_VBASE;
    uint64_t g   = off >> 30;           /* guest PD index (0..3)          */
    uint64_t i   = (off >> 21) & 0x1FF; /* 2 MB slot within the PD        */

    if (uaos_guest_pd[g][i] & PAGE_PRESENT)
        return 0;   /* already committed — a real fault, let it panic */

    if (uaos_pool_committed >= GUEST_POOL_PAGES) {
        extern void kprint(const char *s);
        kprint("[VM] guest window backing pool exhausted\n");
        return -1;
    }

    uint64_t phys = uaos_pool_base + (uint64_t)uaos_pool_committed * PAGE_2MB_SIZE;
    uaos_pool_committed++;

    memset((void *)(uintptr_t)phys, 0, PAGE_2MB_SIZE);
    uaos_guest_pd[g][i] = phys | PAGE_PRESENT | PAGE_WRITABLE
                               | PAGE_USER | PAGE_HUGE;
    __asm__ volatile ("invlpg (%0)" :: "r"(fault_addr) : "memory");
    return 1;
}

/* -----------------------------------------------------------------------
 * UAOS_VM_ReleaseGuestWindow — tear down the window and reset the pool
 * ----------------------------------------------------------------------- */

void UAOS_VM_ReleaseGuestWindow(void)
{
    if (!uaos_guest_win_active) return;

    for (int g = 0; g < (int)GUEST_WIN_PD_COUNT; g++) {
        for (int i = 0; i < PD_ENTRIES; i++) uaos_guest_pd[g][i] = 0;
        uaos_pdpt[GUEST_WIN_PDPT_INDEX + g] = 0;
    }
    uaos_guest_win_active = 0;
    uaos_pool_committed   = 0;

    UAOS_VM_FlushTLB();
}
