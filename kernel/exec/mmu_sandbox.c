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
}

/* -----------------------------------------------------------------------
 * UAOS_MMU_GetPML4Base — returns the host pointer to the PML4 table,
 * useful for the page fault handler to remap chip-window entries at runtime.
 * ----------------------------------------------------------------------- */

uint64_t *UAOS_MMU_GetPML4Base(void)
{
    return (uint64_t *)uaos_pml4;
}
