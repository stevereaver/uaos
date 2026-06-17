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
#include "task.h"

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
#define THUNK_ALLOC_SIGNAL   11
#define THUNK_FREE_SIGNAL    12
#define THUNK_ALLOC_VEC      13
#define THUNK_FREE_VEC       14

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
    UaosRomModule *m = UAOS_ROM_Find(name);
    if (m && version <= m->version) {
        cpu->d[0] = m->amiga_base;
    } else {
        cpu->d[0] = 0;
    }
}

static void stub_AllocMem(M68kCPUState *cpu)
{
    uint32_t byte_size   = cpu->d[0];
    uint32_t requirements = cpu->d[1];
    fprintf(stderr, "[THUNK] AllocMem(%u, 0x%08X)\n", byte_size, requirements);
    extern void dos_AllocMem_glue(uint32_t size, uint32_t reqs, uint32_t *out_addr);
    uint32_t addr = 0;
    dos_AllocMem_glue(byte_size, requirements, &addr);
    cpu->d[0] = addr;
}

static void stub_FreeMem(M68kCPUState *cpu)
{
    uint32_t mem_block = cpu->a[1];
    uint32_t byte_size = cpu->d[0];
    fprintf(stderr, "[THUNK] FreeMem(0x%08X, %u)\n", mem_block, byte_size);
    extern void dos_FreeMem_glue(uint32_t addr, uint32_t size);
    dos_FreeMem_glue(mem_block, byte_size);
}

static void stub_CloseLibrary(M68kCPUState *cpu)
{
    uint32_t lib = cpu->a[1];
    fprintf(stderr, "[THUNK] CloseLibrary(0x%08X)\n", lib);
}

static void stub_FindTask(M68kCPUState *cpu)
{
    uint32_t name_ptr = cpu->a[1];
    UaosTask *t = NULL;
    if (name_ptr != 0) {
        const char *name = (const char *)UAOS_AMIGA_TO_HOST(name_ptr);
        t = Task_FindByName(name);
    } else {
        t = Task_Current();
    }
    if (t && t->type == TASK_TYPE_M68K) {
        cpu->d[0] = t->m68k_task_struct;
    } else if (t) {
        cpu->d[0] = (uint32_t)(uintptr_t)t;
    } else {
        cpu->d[0] = 0;
    }
}

/* AmigaOS Task struct offsets (from exec/tasks.h) */
#define TASK_LN_NAME_OFF   10  /* ln_Name at offset 10 within Node */
#define TASK_TC_FLAGS_OFF  14  /* tc_Flags after Node header */
#define TASK_TC_STATE_OFF  15  /* tc_State */

static void stub_AddTask(M68kCPUState *cpu)
{
    uint32_t task_addr = cpu->a[1];
    uint32_t init_pc = cpu->a[2];
    uint32_t final_pc = cpu->a[3];
    fprintf(stderr, "[THUNK] AddTask(task=0x%08X, initPC=0x%08X, finalPC=0x%08X)\n",
            task_addr, init_pc, final_pc);

    /* Read task name from guest RAM (ln_Name is at offset 10 in Node) */
    uint32_t name_addr = 0;
    if (task_addr + TASK_LN_NAME_OFF + 4 <= 0xFFFFFFFFu) {
        name_addr = ((uint32_t *)UAOS_AMIGA_TO_HOST(task_addr + TASK_LN_NAME_OFF))[0];
    }
    char task_name[32] = "M68kTask";
    if (name_addr) {
        const char *name_ptr = (const char *)UAOS_AMIGA_TO_HOST(name_addr);
        int i = 0;
        while (i < 31 && name_ptr[i]) { task_name[i] = name_ptr[i]; i++; }
        task_name[i] = '\0';
    }

    /* Read priority from ln_Pri (offset 9 in Node) */
    int8_t pri = 0;
    if (task_addr + 9 <= 0xFFFFFFFFu) {
        pri = ((int8_t *)UAOS_AMIGA_TO_HOST(task_addr + 9))[0];
    }

    /* Create M68k task using Task_CreateM68k with init_pc as entry point.
     * The task struct is already set up in guest RAM; we create the host-side
     * wrapper and set the entry point to init_pc. */
    UaosTask *t = Task_CreateM68k(task_name, pri, NULL, 0, NULL, NULL);
    if (t) {
        t->m68k_task_struct = task_addr;
        t->m68k_entry = init_pc;
        cpu->d[0] = 0; /* Success */
    } else {
        cpu->d[0] = (uint32_t)-1; /* Failure */
    }
    (void)final_pc; /* Not used in current implementation */
}

static void stub_RemTask(M68kCPUState *cpu)
{
    uint32_t task_addr = cpu->a[1];
    fprintf(stderr, "[THUNK] RemTask(0x%08X)\n", task_addr);

    /* Find task by guest address and mark as removed */
    UaosTask *t = Task_FindByM68kAddr(task_addr);
    if (t) {
        t->tc_State = TASK_REMOVED;
        /* Also update the guest task struct if accessible */
        if (task_addr + TASK_TC_STATE_OFF <= 0xFFFFFFFFu) {
            uint8_t *state_ptr = (uint8_t *)UAOS_AMIGA_TO_HOST(task_addr + TASK_TC_STATE_OFF);
            *state_ptr = TASK_REMOVED;
        }
        cpu->d[0] = 0; /* Success */
    } else {
        cpu->d[0] = (uint32_t)-1; /* Failure - task not found */
    }
}

static void stub_Wait(M68kCPUState *cpu)
{
    uint32_t sigmask = cpu->d[0];
    cpu->d[0] = Wait(sigmask);
}

static void stub_Signal(M68kCPUState *cpu)
{
    uint32_t task_addr = cpu->a[1];
    uint32_t sigmask   = cpu->d[0];
    UaosTask *t = Task_FindByM68kAddr(task_addr);
    if (!t) t = Task_Current();
    Signal(t, sigmask);
}

static void stub_SetFunction(M68kCPUState *cpu)
{
    fprintf(stderr, "[THUNK] SetFunction(lib=0x%08X, offset=0x%08X, entry=0x%08X)\n",
            cpu->a[1], cpu->a[0], cpu->d[0]);
    cpu->d[0] = 0;
}

static void stub_AllocSignal(M68kCPUState *cpu)
{
    int32_t signal_num = (int32_t)cpu->d[0];
    UaosTask *t = Task_Current();
    if (!t) { cpu->d[0] = (uint32_t)-1; return; }

    if (signal_num == -1) {
        /* Allocate any available signal bit */
        uint32_t alloc_mask = t->tc_SigAlloc;
        for (int i = 0; i < 32; i++) {
            if ((alloc_mask >> i) & 1) {
                t->tc_SigAlloc &= ~(1U << i);
                cpu->d[0] = i;
                return;
            }
        }
        cpu->d[0] = (uint32_t)-1;
    } else if (signal_num >= 0 && signal_num < 32) {
        /* Allocate specific signal bit */
        if ((t->tc_SigAlloc >> signal_num) & 1) {
            t->tc_SigAlloc &= ~(1U << signal_num);
            cpu->d[0] = signal_num;
        } else {
            cpu->d[0] = (uint32_t)-1;
        }
    } else {
        cpu->d[0] = (uint32_t)-1;
    }
}

static void stub_FreeSignal(M68kCPUState *cpu)
{
    uint32_t signal_num = cpu->d[0];
    UaosTask *t = Task_Current();
    if (!t) { cpu->d[0] = (uint32_t)-1; return; }

    if (signal_num < 32) {
        t->tc_SigAlloc |= (1U << signal_num);
        cpu->d[0] = 0;
    } else {
        cpu->d[0] = (uint32_t)-1;
    }
}

static void stub_AllocVec(M68kCPUState *cpu)
{
    uint32_t byte_size   = cpu->d[0];
    uint32_t requirements = cpu->d[1];
    fprintf(stderr, "[THUNK] AllocVec(%u, 0x%08X)\n", byte_size, requirements);
    extern void dos_AllocMem_glue(uint32_t size, uint32_t reqs, uint32_t *out_addr);
    uint32_t addr = 0;
    dos_AllocMem_glue(byte_size, requirements, &addr);
    cpu->d[0] = addr;
}

static void stub_FreeVec(M68kCPUState *cpu)
{
    uint32_t mem_block = cpu->a[1];
    fprintf(stderr, "[THUNK] FreeVec(0x%08X)\n", mem_block);
    extern void dos_FreeMem_glue(uint32_t addr, uint32_t size);
    dos_FreeMem_glue(mem_block, 0);
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
        case THUNK_ALLOC_SIGNAL:   stub_AllocSignal(cpu);   break;
        case THUNK_FREE_SIGNAL:    stub_FreeSignal(cpu);    break;
        case THUNK_ALLOC_VEC:      stub_AllocVec(cpu);      break;
        case THUNK_FREE_VEC:       stub_FreeVec(cpu);       break;
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
