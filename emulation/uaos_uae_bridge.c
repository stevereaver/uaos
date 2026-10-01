/*
 * uaos_uae_bridge.c — UAOS Emulation-to-Native Kernel Lifecycle Bridge
 *
 * This module sits between the headless M68k emulator core (Musashi)
 * and the UAOS native kernel subsystems.  It is responsible for:
 *
 *   1. Allocating and managing the 4 GB guest physical RAM window.
 *   2. Loading ROM patches from rom_traps.s into the guest address space.
 *   3. Wiring the emulator's ILLEGAL-opcode callback to UAOS_HandleThunk.
 *   4. Starting the emulator run-loop and handling clean shutdown.
 *
 * Integration contract:
 *   - The caller must provide an emulator context struct via
 *     UAOS_Bridge_SetEmulatorCtx().
 *   - The emulator must invoke UAOS_Bridge_IllegalOpcode() on every ILLEGAL
 *     opcode it decodes instead of raising an unhandled exception.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Forward declarations for subsystem APIs
 * ----------------------------------------------------------------------- */

/* thunk_handler.c */
typedef struct {
    uint32_t d[8];
    uint32_t a[8];
    uint32_t pc;
    uint16_t sr;
} M68kCPUState;

extern int  UAOS_HandleThunk(M68kCPUState *cpu);
extern void UAOS_SetRamBase(uint8_t *base);
extern void UAOS_Glue_SetRamBase(uint8_t *base);

/* rom_modules.c */
extern void UAOS_ROM_RegisterAll(void);

/* mmu_sandbox.c — demand-paged VA window used as the guest RAM.
 * UAOS_MMU_Init() must already have run (done by the kernel at boot). */
extern void *UAOS_VM_ReserveGuestWindow(void);
extern void  UAOS_VM_ReleaseGuestWindow(void);

/* kernel logging (uaos_kernel_main.c) */
extern void kprint(const char *s);
extern void kprinthex(uint64_t v);

/* -----------------------------------------------------------------------
 * Guest physical RAM window — 4 GB
 * ----------------------------------------------------------------------- */

#define UAOS_GUEST_RAM_SIZE   (4ULL * 1024 * 1024 * 1024)

static uint8_t *uaos_guest_ram = NULL;

/* -----------------------------------------------------------------------
 * Emulator context opaque handle — replaced by the real UAE struct at link
 * time.  We store a void* so this file does not depend on UAE internals.
 * ----------------------------------------------------------------------- */

static void *uaos_emu_ctx = NULL;

/* -----------------------------------------------------------------------
 * Trap table entry — mirrors the .rodata section in rom_traps.s
 * ----------------------------------------------------------------------- */

typedef struct {
    uint32_t stub_addr;   /* 32-bit guest address of the breakout stub      */
    uint16_t func_idx;    /* function index identifier                       */
} UaosTrapEntry;

/* -----------------------------------------------------------------------
 * UAOS_Bridge_SetEmulatorCtx — store the emulator context handle
 * ----------------------------------------------------------------------- */

void UAOS_Bridge_SetEmulatorCtx(void *ctx)
{
    uaos_emu_ctx = ctx;
}

/* -----------------------------------------------------------------------
 * UAOS_Bridge_Init — one-time initialisation, call before the run-loop
 *
 * Returns 0 on success, non-zero on failure.
 * ----------------------------------------------------------------------- */

int UAOS_Bridge_Init(void)
{
    kprint("[BRIDGE] Initialising UAOS kernel bridge\n");

    /* Reserve the 4 GB guest VA window.  This is a pure VA reservation:
     * no contiguous physical allocation is needed because the MMU sandbox
     * commits 2 MB backing pages on demand through the #PF handler, and
     * committed pages arrive zeroed. */
    uaos_guest_ram = (uint8_t *)UAOS_VM_ReserveGuestWindow();
    if (uaos_guest_ram == NULL) {
        kprint("[BRIDGE] FATAL: guest VA window reservation failed\n");
        return -1;
    }
    kprint("[BRIDGE] Guest RAM window: ");
    kprinthex((uint64_t)(uintptr_t)uaos_guest_ram);
    kprint(" – ");
    kprinthex((uint64_t)(uintptr_t)uaos_guest_ram + UAOS_GUEST_RAM_SIZE - 1);
    kprint("\n");

    /* Pass RAM base to the thunk translation layer and the M68k glue      */
    UAOS_SetRamBase(uaos_guest_ram);
    UAOS_Glue_SetRamBase(uaos_guest_ram);

    /* ROM modules are registered by the kernel before bridge init —
     * re-registering here would double-register every module and rebuild
     * the BOOPSI class image against the new g_ram base, corrupting the
     * boot image that per-task M68k guests mirror from g_default_ram.   */

    kprint("[BRIDGE] Initialisation complete\n");
    return 0;
}

/* -----------------------------------------------------------------------
 * UAOS_Bridge_PostInitProbe — verify the guest window is actually backed
 *
 * Must be called after the #PF handler is installed (the window's pages
 * are non-present until first touch, so a probe before the IDT exists
 * would triple-fault).  First-touch write/read-back inside several 2 MB
 * slots drives the demand-commit path in UAOS_VM_GuestWindowFault().
 *
 * Returns 0 on success, -1 if no window was reserved, -2 on read-back
 * mismatch.
 * ----------------------------------------------------------------------- */

int UAOS_Bridge_PostInitProbe(void)
{
    if (uaos_guest_ram == NULL) return -1;

    static const uint32_t probe_offsets[] =
        { 0x00000000u, 0x001FFFFCu, 0x00200000u, 0x00FFFFFCu };
    volatile uint8_t *win = uaos_guest_ram;

    for (unsigned int i = 0; i < sizeof(probe_offsets) / sizeof(probe_offsets[0]); i++) {
        uint32_t off = probe_offsets[i];
        win[off] = 0xA5;
        if (win[off] != 0xA5) return -2;
        win[off] = 0;
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * UAOS_Bridge_IllegalOpcode — callback hooked into the emulator core
 *
 * The emulator must call this function whenever it decodes an ILLEGAL
 * opcode (0x4AFC).  cpu must be the live register state of the guest.
 *
 * Returns:
 *   0   UAOS thunk was dispatched — emulator should continue execution
 *  -1   Not a UAOS trap — emulator should raise an Amiga-level exception
 * ----------------------------------------------------------------------- */

int UAOS_Bridge_IllegalOpcode(M68kCPUState *cpu)
{
    return UAOS_HandleThunk(cpu);
}

/* -----------------------------------------------------------------------
 * UAOS_Bridge_Shutdown — clean up resources on emulator exit
 * ----------------------------------------------------------------------- */

void UAOS_Bridge_Shutdown(void)
{
    kprint("[BRIDGE] Shutdown initiated\n");

    if (uaos_guest_ram != NULL) {
        UAOS_VM_ReleaseGuestWindow();
        uaos_guest_ram = NULL;
    }

    uaos_emu_ctx = NULL;
    kprint("[BRIDGE] Shutdown complete\n");
}

/* -----------------------------------------------------------------------
 * UAOS_Bridge_GetGuestRAM — returns the host pointer to the guest RAM base
 * for direct memory access by other kernel subsystems.
 * ----------------------------------------------------------------------- */

uint8_t *UAOS_Bridge_GetGuestRAM(void)
{
    return uaos_guest_ram;
}
