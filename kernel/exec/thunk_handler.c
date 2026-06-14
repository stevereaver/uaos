/*
 * thunk_handler.c — UAOS Native ABI Thunk Translation Engine
 *
 * Processes JIT core context state upon encountering an ILLEGAL breakout
 * trap emitted by rom_traps.s.  Validates the 0x414D signature, dispatches
 * to the appropriate native C stub, and advances the guest PC by 6 bytes
 * (ILLEGAL word + signature word + index word) past the breakout sequence.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "rom_modules.h"

/* M68kCPUState is defined in rom_modules.h (shared with ROM stubs) */

/* -----------------------------------------------------------------------
 * UAOS_AMIGA_TO_HOST — maps a 32-bit Amiga-space address to the host
 * linear address by adding the physical RAM base allocated for the guest.
 * ram_base must point to the start of the 4 GB guest memory window.
 * ----------------------------------------------------------------------- */

static uint8_t *uaos_ram_base = NULL;

#define UAOS_AMIGA_TO_HOST(amiga_addr) \
    ((void *)((uintptr_t)(uaos_ram_base) + (uint32_t)(amiga_addr)))

void UAOS_SetRamBase(uint8_t *base)
{
    uaos_ram_base = base;
}

/* -----------------------------------------------------------------------
 * UAOS thunk function index assignments — must match rom_traps.s
 * ----------------------------------------------------------------------- */

#define THUNK_OPEN_LIBRARY    1
#define THUNK_ALLOC_MEM       2
#define THUNK_FREE_MEM        3
#define THUNK_CLOSE_LIBRARY   4
#define THUNK_FIND_TASK       5
#define THUNK_ADD_TASK        6
#define THUNK_REM_TASK        7
#define THUNK_WAIT            8
#define THUNK_SIGNAL          9
#define THUNK_SET_FUNCTION   10

/* -----------------------------------------------------------------------
 * Signature constant embedded immediately after every ILLEGAL opcode
 * ----------------------------------------------------------------------- */

#define UAOS_TRAP_SIGNATURE  0x414D   /* "AM" */
#define UAOS_BREAKOUT_SIZE   6        /* ILLEGAL(2) + SIG(2) + IDX(2)     */

/* -----------------------------------------------------------------------
 * Native stub implementations — minimal skeletons; replace with full
 * Exec library logic as the kernel matures.
 * ----------------------------------------------------------------------- */

static void stub_OpenLibrary(M68kCPUState *cpu)
{
    const char *name = (const char *)UAOS_AMIGA_TO_HOST(cpu->a[1]);
    uint32_t    version = cpu->d[0];
    fprintf(stderr, "[THUNK] OpenLibrary(\"%s\", %u)\n", name, version);
    cpu->d[0] = 0; /* NULL — library not yet resident; extend as needed  */
}

static void stub_AllocMem(M68kCPUState *cpu)
{
    uint32_t byte_size   = cpu->d[0];
    uint32_t requirements = cpu->d[1];
    fprintf(stderr, "[THUNK] AllocMem(%u, 0x%08X)\n", byte_size, requirements);
    cpu->d[0] = 0; /* NULL on failure stub                                */
}

static void stub_FreeMem(M68kCPUState *cpu)
{
    uint32_t mem_block = cpu->a[1];
    uint32_t byte_size = cpu->d[0];
    fprintf(stderr, "[THUNK] FreeMem(0x%08X, %u)\n", mem_block, byte_size);
}

static void stub_CloseLibrary(M68kCPUState *cpu)
{
    uint32_t lib = cpu->a[1];
    fprintf(stderr, "[THUNK] CloseLibrary(0x%08X)\n", lib);
}

static void stub_FindTask(M68kCPUState *cpu)
{
    uint32_t name_ptr = cpu->a[1];
    if (name_ptr != 0) {
        const char *name = (const char *)UAOS_AMIGA_TO_HOST(name_ptr);
        fprintf(stderr, "[THUNK] FindTask(\"%s\")\n", name);
    } else {
        fprintf(stderr, "[THUNK] FindTask(NULL) — current task\n");
    }
    cpu->d[0] = 0;
}

static void stub_AddTask(M68kCPUState *cpu)
{
    fprintf(stderr, "[THUNK] AddTask(task=0x%08X, initPC=0x%08X, finalPC=0x%08X)\n",
            cpu->a[1], cpu->a[2], cpu->a[3]);
    cpu->d[0] = 0;
}

static void stub_RemTask(M68kCPUState *cpu)
{
    fprintf(stderr, "[THUNK] RemTask(0x%08X)\n", cpu->a[1]);
}

static void stub_Wait(M68kCPUState *cpu)
{
    fprintf(stderr, "[THUNK] Wait(sigmask=0x%08X)\n", cpu->d[0]);
    cpu->d[0] = 0;
}

static void stub_Signal(M68kCPUState *cpu)
{
    fprintf(stderr, "[THUNK] Signal(task=0x%08X, signals=0x%08X)\n",
            cpu->a[1], cpu->d[0]);
}

static void stub_SetFunction(M68kCPUState *cpu)
{
    fprintf(stderr, "[THUNK] SetFunction(lib=0x%08X, offset=0x%08X, entry=0x%08X)\n",
            cpu->a[1], cpu->a[0], cpu->d[0]);
    cpu->d[0] = 0;
}

/* -----------------------------------------------------------------------
 * UAOS_HandleThunk — main dispatcher
 *
 * Called by the JIT/emulator core each time an ILLEGAL opcode is decoded.
 * The guest PC must already point AT the ILLEGAL opcode word when this
 * function is entered.
 *
 * Returns:
 *   0  on successful dispatch (PC advanced by UAOS_BREAKOUT_SIZE)
 *  -1  if the opcode is not a UAOS trap (signature mismatch)
 *  -2  if the function index is unknown
 * ----------------------------------------------------------------------- */

int UAOS_HandleThunk(M68kCPUState *cpu)
{
    if (uaos_ram_base == NULL) {
        fprintf(stderr, "[THUNK] ERROR: RAM base not set — call UAOS_SetRamBase() first\n");
        return -1;
    }

    /* The breakout sequence starts at cpu->pc (ILLEGAL word already decoded).
     * Layout in guest memory: [4AFC][414D][funcidx]
     * We read signature and index from the two words following the ILLEGAL.  */
    const uint16_t *words =
        (const uint16_t *)UAOS_AMIGA_TO_HOST(cpu->pc + 2);

    uint16_t signature = words[0];
    uint16_t func_idx  = words[1];

    if (signature != UAOS_TRAP_SIGNATURE) {
        /* Not a UAOS breakout — let the emulator core handle it normally   */
        return -1;
    }

    switch (func_idx) {
        case THUNK_OPEN_LIBRARY:   stub_OpenLibrary(cpu);   break;
        case THUNK_ALLOC_MEM:      stub_AllocMem(cpu);      break;
        case THUNK_FREE_MEM:       stub_FreeMem(cpu);       break;
        case THUNK_CLOSE_LIBRARY:  stub_CloseLibrary(cpu);  break;
        case THUNK_FIND_TASK:      stub_FindTask(cpu);      break;
        case THUNK_ADD_TASK:       stub_AddTask(cpu);       break;
        case THUNK_REM_TASK:       stub_RemTask(cpu);       break;
        case THUNK_WAIT:           stub_Wait(cpu);          break;
        case THUNK_SIGNAL:         stub_Signal(cpu);        break;
        case THUNK_SET_FUNCTION:   stub_SetFunction(cpu);   break;
        default:
            fprintf(stderr, "[THUNK] Unknown function index %u at PC=0x%08X\n",
                    func_idx, cpu->pc);
            return -2;
    }

    /* Advance guest PC past the entire 6-byte breakout sequence.
     * The RTS at the end of the stub is handled by the normal flow.        */
    cpu->pc += UAOS_BREAKOUT_SIZE;
    return 0;
}
