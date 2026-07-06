/* exec_task.c — UAOS Exec-compatible task management
 *
 * Implements AddTask, RemTask, FindTask, SetTaskPri for both native
 * and M68k guest tasks.  M68k tasks are backed by a native wrapper that
 * runs m68k_execute() in time-sliced chunks.
 */

#include "task.h"
#include "amiga_task.h"
#include <stdint.h>
#include <stddef.h>

/* Musashi context size — queried at runtime */
extern unsigned int m68k_context_size(void);
extern unsigned int m68k_get_context(void *dst);
extern unsigned int m68k_get_reg(void *context, int reg);
#define M68K_REG_PC_S 16  /* Musashi M68K_REG_PC enum value */
extern void m68k_set_context(void *src);
extern int m68k_execute(int num_cycles);
extern unsigned int m68k_cycles_run(void);
extern void m68k_end_timeslice(void);
extern void m68k_init(void);
extern void m68k_set_cpu_type(int type);
extern void m68k_pulse_reset(void);
extern unsigned int m68k_read_memory_32(unsigned int addr);
extern void m68k_write_memory_32(unsigned int addr, unsigned int val);
extern void m68k_set_reg(int reg, unsigned int val);

/* From uaos_m68k_glue.c — guest RAM and binary loader */
#include "../../emulation/uaos_emu.h"
#include "chipset/chip_emu.h"
extern uint8_t *g_ram;
extern int g_emu_halted;
extern uint32_t g_uaos_heap_ptr;
extern uint64_t g_m68k_cycles;
extern uint32_t heap_alloc(uint32_t size);

/* Forward declarations from uaos_m68k_glue.c */
extern void install_library_tables(void);
extern uint32_t hunk_load(const uint8_t *binary, uint32_t bin_size);
extern void UAOS_Emu_SetCwd(const char *cwd);

/* Stack top for M68k guest */
#define STACK_TOP  0x1F0000
#define PROG_BASE  0x001000

/* Per-task M68k RAM pool */
#define MAX_M68K_TASKS  4
static uint8_t g_ram_pool[MAX_M68K_TASKS][GUEST_RAM_SIZE] __attribute__((section(".guest_ram"), aligned(4096)));
static uint8_t g_ram_used[MAX_M68K_TASKS] = {0};

/* Musashi cycle globals (needed for context save/restore) */
extern int m68ki_initial_cycles;
extern int m68ki_remaining_cycles;

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static int alloc_m68k_ram_slot(void)
{
    for (int i = 0; i < MAX_M68K_TASKS; i++) {
        if (!g_ram_used[i]) {
            g_ram_used[i] = 1;
            return i;
        }
    }
    return -1;
}

static void free_m68k_ram_slot(uint8_t *ram)
{
    for (int i = 0; i < MAX_M68K_TASKS; i++) {
        if (g_ram_pool[i] == ram) {
            g_ram_used[i] = 0;
            return;
        }
    }
}

/* Big-endian helpers for guest RAM */
static void guest_w32(uint32_t addr, uint32_t val)
{
    g_ram[addr + 0] = (uint8_t)(val >> 24);
    g_ram[addr + 1] = (uint8_t)(val >> 16);
    g_ram[addr + 2] = (uint8_t)(val >>  8);
    g_ram[addr + 3] = (uint8_t)(val      );
}

static uint32_t guest_r32(uint32_t addr)
{
    return ((uint32_t)g_ram[addr + 0] << 24)
         | ((uint32_t)g_ram[addr + 1] << 16)
         | ((uint32_t)g_ram[addr + 2] <<  8)
         | ((uint32_t)g_ram[addr + 3]      );
}

static void guest_memset(uint32_t addr, uint8_t c, uint32_t n)
{
    for (uint32_t i = 0; i < n && addr + i < GUEST_RAM_SIZE; i++)
        g_ram[addr + i] = c;
}

static void guest_memcpy(uint32_t dst, const uint8_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n && dst + i < GUEST_RAM_SIZE; i++)
        g_ram[dst + i] = src[i];
}

/* -------------------------------------------------------------------------
 * M68k wrapper task — runs a loaded binary in time-sliced chunks
 * ------------------------------------------------------------------------- */

/* Stub addresses for library dispatch (from uaos_m68k_glue.c) */
#define EXEC_BASE       0x0300
#define FAKE_LIB_BASE   0xF000
#define DOS_STDIN_BPTR  0x00000200
#define DOS_STDOUT_BPTR 0x00000204

/* CLI struct offsets used by the M68k wrapper to publish command-line info */
#define CLI_COMMAND_NAME_OFFSET 0x10
#define CLI_COMMAND_LINE_OFFSET 0x2C

static void m68k_wrapper_entry(void *arg)
{
    UaosTask *task = (UaosTask *)arg;
    const uint8_t *binary = (const uint8_t *)task->tc_UserData;

    /* Switch to this task's guest RAM */
    g_ram = task->m68k_ram;

    /* Set output callback */
    g_print = (GluePrintFn)task->m68k_print_fn;

    /* Clear and initialise this task's RAM */
    for (uint32_t i = 0; i < GUEST_RAM_SIZE; i++)
        g_ram[i] = 0;

    /* Install library jump tables (per-task) */
    install_library_tables();

    /* Load binary */
    g_uaos_heap_ptr = PROG_BASE;
    uint32_t entry = hunk_load(binary, task->m68k_bin_size);
    if (!entry) {
        extern void kprint(const char *);
        kprint("[M68K] hunk_load failed, exiting\n");
        Task_Exit();
    }
    {
        extern void kprint(const char *);
        kprint("[M68K] hunk loaded, entry point\n");
    }

    /* Build command line in guest RAM (matches UAOS_Emu_LoadAndRun_Internal) */
    uint32_t sp = STACK_TOP;
    char cmdline[256];
    int cmdlen = 0;
    const char **argv = task->m68k_argv;
    if (argv) {
        for (int i = 1; argv[i] && cmdlen < 254; i++) {
            if (i > 1 && cmdlen < 254) cmdline[cmdlen++] = ' ';
            for (int j = 0; argv[i][j] && cmdlen < 254; j++)
                cmdline[cmdlen++] = argv[i][j];
        }
    }
    cmdline[cmdlen] = '\n';
    cmdlen++;
    cmdline[cmdlen] = '\0';

    /* Place cmdline string just below SP */
    sp -= (uint32_t)((cmdlen + 2) & ~1u);
    uint32_t cmdline_ptr = sp;
    guest_memcpy(cmdline_ptr, (const uint8_t *)cmdline, (uint32_t)cmdlen);

    /* Build a BSTR version for GetArgStr (byte[0]=len, byte[1..len]=chars) */
    sp -= (uint32_t)((cmdlen + 2 + 4) & ~3u);
    uint32_t bstr_ptr = sp;
    g_ram[bstr_ptr] = (uint8_t)(cmdlen < 255 ? cmdlen : 255);
    guest_memcpy(bstr_ptr + 1, (const uint8_t *)cmdline, (uint32_t)cmdlen);
    g_cmdline_bptr = bstr_ptr >> 2;

    /* Build a BSTR for the command name (argv[0] / task name) */
    sp -= 8;
    uint32_t cmdname_bstr_ptr = sp;
    const char *cmdname = task->m68k_argv[0] ? task->m68k_argv[0] : task->m68k_name;
    uint8_t cmdname_len = 0;
    while (cmdname_len < 15 && cmdname[cmdname_len]) cmdname_len++;
    g_ram[cmdname_bstr_ptr] = cmdname_len;
    for (int i = 0; i < cmdname_len; i++)
        g_ram[cmdname_bstr_ptr + 1 + i] = (uint8_t)cmdname[i];
    uint32_t cmdname_bptr = cmdname_bstr_ptr >> 2;

    /* Build minimal Process struct */
    uint32_t proc_addr = 0x10000;
    uint32_t cli_addr  = 0x10100;
    guest_memset(proc_addr, 0, 0x100);
    guest_memset(cli_addr,  0, 0x80);

    /* Link the Process struct to the host-side task so that
     * Task_FindByM68kAddr() can locate this task when Intuition needs to
     * signal it (e.g. IDCMP_CLOSEWINDOW).  Without this, m68k_task_struct
     * stays 0 and every M68k task collides on the same lookup key. */
    task->m68k_task_struct = proc_addr;

    /* pr_CLI = BPTR to CLI */
    guest_w32(proc_addr + PR_CLI_OFFSET, cli_addr >> 2);
    /* pr_CIS = stdin BPTR */
    guest_w32(proc_addr + PR_CIS_OFFSET, DOS_STDIN_BPTR);
    /* pr_COS = stdout BPTR */
    guest_w32(proc_addr + PR_COS_OFFSET, DOS_STDOUT_BPTR);
    /* Minimal CLI struct (just needs to be non-zero) */
    g_ram[cli_addr] = 0x01;
    /* Publish command name and command line in the CLI struct */
    guest_w32(cli_addr + CLI_COMMAND_NAME_OFFSET, cmdname_bptr);
    guest_w32(cli_addr + CLI_COMMAND_LINE_OFFSET, g_cmdline_bptr);

    /* Store Process pointer at ExecBase+0x114 */
    guest_w32(EXEC_BASE + 0x114, proc_addr);
    /* Store SysBase at absolute address 4 */
    guest_w32(4, EXEC_BASE);

    /* Set up M68k CPU */
    m68k_init();
    m68k_set_cpu_type(1);  /* M68K_CPU_TYPE_68000 */
    /* Re-register the ILLEGAL instruction callback — m68k_init() clears it. */
    extern int m68k_illg_instr_callback(int opcode);
    extern void m68k_set_illg_instr_callback(int (*cb)(int));
    m68k_set_illg_instr_callback(m68k_illg_instr_callback);

    /* Patch reset vectors */
    m68k_write_memory_32(0, sp);
    m68k_write_memory_32(4, entry);
    m68k_pulse_reset();
    m68k_write_memory_32(4, EXEC_BASE);

    /* CLI entry registers per Amiga CLI convention */
    m68k_set_reg(0, cmdline_ptr);        /* A0 = command line pointer */
    m68k_set_reg(8, (unsigned int)cmdlen); /* D0 = command line length */

    /* Run in time-sliced chunks */
    g_emu_halted = 0;
    task->m68k_halted = 0;
    task->m68k_entry = entry;
    task->m68k_stack_top = sp;

    /* Save initial context so ISR can restore it on first switch */
    unsigned int ctx_size = m68k_context_size();
    if (task->m68k_context_buf && ctx_size > 0) {
        m68k_get_context(task->m68k_context_buf);
        task->m68k_initial_cycles = 0;
        task->m68k_remaining_cycles = 0;
    }

    while (!task->m68k_halted) {
        /* Execute ~1 ms worth of cycles at ~7 MHz ≈ 7000 cycles */
        m68k_execute(10000);
        g_m68k_cycles += (uint64_t)m68k_cycles_run();
        chip_emu_run_to_cycle(g_m68k_cycles);

        /* Check if the binary called Exit */
        if (g_emu_halted) {
            task->m68k_halted = 1;
            break;
        }

        /* Voluntary yield — lets the timer ISR preempt us cleanly */
        __asm__ volatile ("pause");
    }

    Task_Exit();
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

UaosTask *Task_CreateM68k(const char *name, int8_t pri,
                           const uint8_t *binary, uint32_t bin_size,
                           const char **argv,
                           void (*print_fn)(const char *))
{
    int slot = alloc_m68k_ram_slot();
    if (slot < 0) return NULL;

    /* Allocate Musashi context buffer */
    unsigned int ctx_size = m68k_context_size();
    void *ctx_buf = NULL;
    if (ctx_size > 0) {
        /* Use a static pool for context buffers */
        static uint8_t ctx_pool[MAX_M68K_TASKS][4096];
        ctx_buf = ctx_pool[slot];
    }

    Forbid();

    /* Create the native wrapper task */
    UaosTask *t = Task_CreateNative(name, pri, m68k_wrapper_entry, NULL);
    if (!t) {
        free_m68k_ram_slot(g_ram_pool[slot]);
        Permit();
        return NULL;
    }

    t->type = TASK_TYPE_M68K;
    t->m68k_ram = g_ram_pool[slot];
    t->m68k_context_size = ctx_size;
    t->m68k_bin_size = bin_size;
    t->m68k_context_buf = ctx_buf;
    t->native_arg = t;                /* pass task pointer to wrapper */
    t->tc_UserData = (void *)binary;  /* binary pointer for wrapper */
    t->m68k_print_fn = (void *)print_fn;

    /* Allocate a private signal bit for WaitTOF() so M68k demos can block
     * on VBlank instead of busy-waiting and starving the idle/WM task. */
    t->m68k_vblank_sig = -1;
    uint32_t alloc_mask = t->tc_SigAlloc;
    for (int i = 0; i < 32; i++) {
        if ((alloc_mask >> i) & 1u) {
            t->tc_SigAlloc &= ~(1u << i);
            t->m68k_vblank_sig = (int8_t)i;
            break;
        }
    }

    /* Copy task name into a persistent per-task buffer. */
    {
        int i = 0;
        while (i < 15 && name[i]) {
            t->m68k_name[i] = name[i];
            i++;
        }
        t->m68k_name[i] = '\0';
        t->ln_Name = t->m68k_name;
    }

    /* Copy argv into a persistent per-task buffer. */
    if (argv) {
        int argc = 0;
        int ai = 0;
        while (argv[argc] && argc < 17) {
            const char *src = argv[argc];
            int len = 0;
            while (src[len] && ai + len < 255) {
                t->m68k_argv_store[ai + len] = src[len];
                len++;
            }
            t->m68k_argv_store[ai + len] = '\0';
            t->m68k_argv[argc] = &t->m68k_argv_store[ai];
            ai += len + 1;
            argc++;
            if (ai >= 256) break;
        }
        t->m68k_argv[argc] = NULL;
    } else {
        t->m68k_argv[0] = NULL;
    }

    /* Patch the synthetic interrupt frame so the first time the task
     * is switched to via Task_SwitchContext, RDI receives the task pointer. */
    uint64_t *frame = (uint64_t *)t->native_rsp;
    frame[9] = (uint64_t)t;           /* RDI slot */

    Permit();
    return t;
}

/* FindTask — AmigaOS compatible */
UaosTask *FindTask(const char *name)
{
    if (!name || !*name) return Task_Current();

    /* TODO: scan all tasks in ready queues + current */
    return Task_Current();
}

/* SetTaskPri — change a task's priority */
void SetTaskPri(UaosTask *task, int newpri)
{
    if (!task) return;
    if (newpri < MIN_PRI) newpri = MIN_PRI;
    if (newpri > MAX_PRI) newpri = MAX_PRI;
    task->ln_Pri = (int8_t)newpri;
    /* TODO: if task is on a ready queue, move it to the new priority queue */
}
