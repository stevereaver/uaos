/*
 * uaos_uae_bridge.c — UAOS Emulation-to-Native Kernel Lifecycle Bridge
 *
 * This module sits between the headless M68k emulator core (derived from
 * UAE / FS-UAE) and the UAOS native kernel subsystems.  It is responsible
 * for:
 *
 *   1. Allocating and managing the 4 GB guest physical RAM window.
 *   2. Loading ROM patches from rom_traps.s into the guest address space.
 *   3. Wiring the emulator's ILLEGAL-opcode callback to UAOS_HandleThunk.
 *   4. Starting the emulator run-loop and handling clean shutdown.
 *
 * Integration contract:
 *   - The caller must provide a UAE-compatible emulator context struct via
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

/* rom_modules.c */
extern void UAOS_ROM_RegisterAll(void);

/* mmu_sandbox.c */
extern void UAOS_MMU_Init(void);

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
    fprintf(stderr, "[BRIDGE] Initialising UAOS kernel bridge\n");

    /* Allocate aligned 4 GB guest RAM window                              */
    uaos_guest_ram = (uint8_t *)aligned_alloc(4096, UAOS_GUEST_RAM_SIZE);
    if (uaos_guest_ram == NULL) {
        fprintf(stderr, "[BRIDGE] FATAL: failed to allocate guest RAM\n");
        return -1;
    }
    memset(uaos_guest_ram, 0, UAOS_GUEST_RAM_SIZE);
    fprintf(stderr, "[BRIDGE] Guest RAM window: %p – %p\n",
            (void *)uaos_guest_ram,
            (void *)(uaos_guest_ram + UAOS_GUEST_RAM_SIZE - 1));

    /* Pass RAM base to the thunk translation layer                        */
    UAOS_SetRamBase(uaos_guest_ram);

    /* Register all built-in ROM library modules                           */
    UAOS_ROM_RegisterAll();

    /* Install MMU sandbox paging tables (bare-metal only; skipped on
     * hosted builds where paging is already managed by the host OS)       */
#if defined(UAOS_BARE_METAL)
    UAOS_MMU_Init();
#endif

    fprintf(stderr, "[BRIDGE] Initialisation complete\n");
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
    fprintf(stderr, "[BRIDGE] Shutdown initiated\n");

    if (uaos_guest_ram != NULL) {
        free(uaos_guest_ram);
        uaos_guest_ram = NULL;
    }

    uaos_emu_ctx = NULL;
    fprintf(stderr, "[BRIDGE] Shutdown complete\n");
}

/* -----------------------------------------------------------------------
 * UAOS_Bridge_GetGuestRAM — returns the host pointer to the guest RAM base
 * for direct memory access by other kernel subsystems.
 * ----------------------------------------------------------------------- */

uint8_t *UAOS_Bridge_GetGuestRAM(void)
{
    return uaos_guest_ram;
}
