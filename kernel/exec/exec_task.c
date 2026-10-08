/* exec_task.c — UAOS Exec-compatible task management
 *
 * Implements AddTask, RemTask, FindTask, SetTaskPri for both native
 * and M68k guest tasks.  M68k tasks are backed by a native wrapper that
 * runs m68k_execute() in time-sliced chunks.
 */

#include "task.h"
#include "amiga_task.h"
#include "chipset/chiptrace.h"
#include "../irq/irq.h"
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
/* Arm the packed-hunk write watch (decrunch-corruption debug).  Poke to 1
 * before launching a guest binary: poke &g_watch_mem 1 FORCE. */
volatile int g_watch_mem = 1;
extern uint32_t g_uaos_heap_ptr;
extern uint64_t g_m68k_cycles;
extern uint32_t heap_alloc(uint32_t size);

/* Forward declarations from uaos_m68k_glue.c */
extern void install_library_tables(void);
extern uint32_t hunk_load(const uint8_t *binary, uint32_t bin_size);
extern uint32_t UAOS_Emu_SetupProcess(uint32_t cmdname_bptr);
extern uint32_t UAOS_Emu_SetupWbLaunch(const UaosWbLaunch *wb,
                                       uint32_t cmdname_bptr);
extern int  UAOS_Emu_WbStartupReplied(void);
extern void UAOS_Emu_SetCwd(const char *cwd);

/* Pending Workbench launch descriptor (UAOS-253) — filled by
 * ExecFile_RunWB() / the desktop icon path and consumed by the next
 * Task_CreateM68k() call, then cleared. */
UaosWbLaunch g_wb_pending;

/* Stack top for M68k guest */
#define STACK_TOP  0x1F0000
/* Must match emulation/uaos_m68k_glue.c — hunks load above the reserved
 * system zone (stub tables + lib bases + loadable libs, 0x1000-0x1A000). */
#define PROG_BASE  0x020000

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

/* Public wrapper called by Task_Exit() to release a dying M68k task's
 * guest RAM slot so it can be reused by future Task_CreateM68k calls. */
void Task_ReleaseM68kRam(UaosTask *t)
{
    if (t && t->type == TASK_TYPE_M68K && t->m68k_ram) {
        /* Detach the dead task's window from the chipset DMA engines
         * before the slot is freed — the PIT-driven copper/blitter/audio/
         * floppy paths remember which window they were launched against
         * and would otherwise scribble into the next tenant (UAOS-247). */
        chip_emu_unbind_ram(t->m68k_ram);
        /* Drop the host-side heap free-list mirror for this window — it is
         * keyed by window pointer, so a stale entry would hand the next
         * tenant the dead task's freelist head (UAOS-247). */
        { extern void UAOS_Heap_ReleaseWindow(uint8_t *ram);
          UAOS_Heap_ReleaseWindow(t->m68k_ram); }
        free_m68k_ram_slot(t->m68k_ram);
        t->m68k_ram = NULL;
    }
}

/* Report M68k guest RAM slot usage to callers outside exec_task.c
 * (used by the kernel memory query API). */
void Task_M68kSlotCount(int *total, int *used)
{
    int u = 0;
    for (int i = 0; i < MAX_M68K_TASKS; i++)
        if (g_ram_used[i]) u++;
    if (total) *total = MAX_M68K_TASKS;
    if (used)  *used  = u;
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

/* Stub addresses for library dispatch (from uaos_m68k_glue.c, UAOS-252) */
#define EXEC_BASE       0x1000
#define FAKE_LIB_BASE   0xF000
#define DOS_STDIN_BPTR  0x00000200
#define DOS_STDOUT_BPTR 0x00000204

/* CLI struct offsets used by the M68k wrapper to publish command-line info */
#define CLI_COMMAND_NAME_OFFSET 0x10
#define CLI_COMMAND_LINE_OFFSET 0x2C

static void m68k_wrapper_entry(void *arg)
{
    UaosTask *task = (UaosTask *)arg;

    /* Switch to this task's guest RAM */
    g_ram = task->m68k_ram;

    /* Set output callback */
    g_print = (GluePrintFn)task->m68k_print_fn;

    /* Disable chipset sync during per-task M68k execution.
     * The chipset emulator uses global blitter/copper state that is
     * shared across all tasks.  Calling chip_emu_run_to_cycle() from
     * per-task memory accesses would cause the blitter to operate on
     * this task's g_ram with addresses set up by another task, causing
     * kernel page faults.  The chipset is driven by the global timer
     * ISR instead. */
    extern int g_chipset_sync_disabled;
    g_chipset_sync_disabled = 1;

    /* The binary was copied into the tail of guest RAM by Task_CreateM68k.
     * Save the offset before we clear the lower portion of guest RAM. */
    uint32_t bin_save = task->m68k_bin_save_off;
    uint32_t bin_size = task->m68k_bin_size;

    /* Clear guest RAM (only the area below the saved binary) */
    for (uint32_t i = 0; i < bin_save; i++)
        g_ram[i] = 0;

    /* Generated library bases were allocated out of the RAM we just
     * cleared — drop the cache before reinstalling the jump tables. */
    { extern void emu_reset_genlibs(void); emu_reset_genlibs(); }

    /* Install library jump tables (per-task) */
    install_library_tables();

    /* Mirror the boot-time BOOPSI class image into this task's RAM.
     * Class objects were allocated in the shared emulator RAM, but class
     * lookup/dispatch dereferences their guest addresses through the
     * current g_ram — without the mirror, find_public_class() reads zeros
     * and NewObjectA() fails in every per-task context. */
    {
        extern void UAOS_Emu_MirrorSharedRegion(uint32_t off, uint32_t len);
        extern void UAOS_Intuition_ClassImageRange(uint32_t *s, uint32_t *e);
        uint32_t cs = 0, ce = 0;
        UAOS_Intuition_ClassImageRange(&cs, &ce);
        if (ce > cs) UAOS_Emu_MirrorSharedRegion(cs, ce - cs);
    }

    /* Load binary — use the saved copy in guest RAM. */
    g_uaos_heap_ptr = PROG_BASE;
    uint32_t entry = hunk_load(g_ram + bin_save, bin_size);
    if (!entry) {
        extern void kprint(const char *);
        kprint("[M68K] hunk_load failed, exiting\n");
        Task_Exit();
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

    /* Place cmdline string just below SP.  BPTRs are (addr >> 2), so every
     * pointer handed to the guest as a BPTR must be 4-byte aligned — keep
     * sp aligned after each reservation (UAOS-237: a 2-aligned cmdline_ptr
     * cascaded into a misaligned cmdname BSTR and produced an empty
     * Process ln_Name). */
    sp -= (uint32_t)((cmdlen + 2) & ~1u);
    sp &= ~3u;
    uint32_t cmdline_ptr = sp;
    guest_memcpy(cmdline_ptr, (const uint8_t *)cmdline, (uint32_t)cmdlen);

    /* Build a BSTR version for GetArgStr (byte[0]=len, byte[1..len]=chars) */
    sp -= (uint32_t)((cmdlen + 2 + 4) & ~3u);
    sp &= ~3u;
    uint32_t bstr_ptr = sp;
    g_ram[bstr_ptr] = (uint8_t)(cmdlen < 255 ? cmdlen : 255);
    guest_memcpy(bstr_ptr + 1, (const uint8_t *)cmdline, (uint32_t)cmdlen);
    g_cmdline_bptr = bstr_ptr >> 2;

    /* Build a BSTR for the command name (argv[0] / task name) — up to
     * 1+15 bytes, so reserve 16. */
    sp -= 16;
    uint32_t cmdname_bstr_ptr = sp;
    const char *cmdname = task->m68k_argv[0] ? task->m68k_argv[0] : task->m68k_name;
    { extern void UAOS_Emu_SetTarCompat(const char *); UAOS_Emu_SetTarCompat(cmdname); }
    uint8_t cmdname_len = 0;
    while (cmdname_len < 15 && cmdname[cmdname_len]) cmdname_len++;
    g_ram[cmdname_bstr_ptr] = cmdname_len;
    for (int i = 0; i < cmdname_len; i++)
        g_ram[cmdname_bstr_ptr + 1 + i] = (uint8_t)cmdname[i];
    uint32_t cmdname_bptr = cmdname_bstr_ptr >> 2;

    /* Build the guest Process/Task/CLI/console-port environment AFTER
     * hunk_load so pr_SegList/cli_Module can point at the loaded seglist.
     * The structs live in the dedicated region at 0x1B000 — outside the
     * program image and every guest allocator (UAOS-237).
     * Workbench launches (UAOS-253) get the WBStartup environment
     * instead: pr_CLI = 0 and a queued WBStartup message on pr_MsgPort. */
    uint32_t proc_addr = task->m68k_is_wb
        ? UAOS_Emu_SetupWbLaunch(&task->m68k_wb, cmdname_bptr)
        : UAOS_Emu_SetupProcess(cmdname_bptr);
    if (!proc_addr) {
        extern void kprint(const char *);
        kprint("[M68K] process setup OOM, exiting\n");
        Task_Exit();
    }
    if (task->m68k_is_wb) {
        /* GetProgramDir()/tool-dir lock for programs that consult it. */
        task->m68k_program_dir = guest_r32(proc_addr + 0x98);
    }

    /* Link the Process struct to the host-side task so that
     * Task_FindByM68kAddr() can locate this task when Intuition needs to
     * signal it (e.g. IDCMP_CLOSEWINDOW).  Without this, m68k_task_struct
     * stays 0 and every M68k task collides on the same lookup key. */
    task->m68k_task_struct = proc_addr;

    /* Store SysBase at absolute address 4 */
    guest_w32(4, EXEC_BASE);

    /* Push return address — DOS Exit stub so RTS ends execution.
     * The stub address = DOS_BASE + LVO_DOS_EXIT = 0x2000 + (-144) = 0x1F70.
     * When the program does RTS at the end, it returns to the Exit stub
     * which triggers the illegal instruction handler and halts. */
    #define DOS_BASE_LOCAL  0x2000
    #define LVO_DOS_EXIT_LOCAL (-144)
    sp -= 4;
    guest_w32(sp, (uint32_t)((int)DOS_BASE_LOCAL + LVO_DOS_EXIT_LOCAL));

    /* Set up M68k CPU */
    m68k_init();
    m68k_set_cpu_type(2);  /* M68K_CPU_TYPE_68020 */
    /* Re-register the ILLEGAL instruction callback — m68k_init() clears it. */
    extern int m68k_illg_instr_callback(int opcode);
    extern void m68k_set_illg_instr_callback(int (*cb)(int));
    m68k_set_illg_instr_callback(m68k_illg_instr_callback);
    /* Same for the PC-ring instruction hook — lives in m68ki_cpu state. */
    extern void uaos_m68k_instr_hook(unsigned int pc);
    extern void m68k_set_instr_hook_callback(void (*cb)(unsigned int));
    m68k_set_instr_hook_callback(uaos_m68k_instr_hook);

    /* Patch reset vectors */
    m68k_write_memory_32(0, sp);
    m68k_write_memory_32(4, entry);
    m68k_pulse_reset();
    m68k_write_memory_32(4, EXEC_BASE);

    /* pulse_reset starts at IPL 7 — drop to supervisor/IPL 0 like a real
     * exec task so chipset autovectors can preempt the guest (UAOS-241). */
    m68k_set_reg(17 /*M68K_REG_SR*/, 0x2000);

    /* CLI entry registers per Amiga CLI convention.
     * In Musashi: M68K_REG_D0=0, M68K_REG_A0=8, M68K_REG_A6=14.
     * A0 = command line pointer, D0 = command line length.
     * A6 = SysBase (ExecBase) — many programs expect this to be pre-set
     * by the loader, especially ACE-compiled binaries. */
    m68k_set_reg(8, cmdline_ptr);        /* A0 = command line pointer */
    m68k_set_reg(0, (unsigned int)cmdlen); /* D0 = command line length */
    m68k_set_reg(14, EXEC_BASE);        /* A6 = EXEC_BASE (SysBase) */

    /* Workbench convention (UAOS-253): argc=0/argv=NULL — startup code
     * fetches the WBStartup message from pr_MsgPort itself. */
    if (task->m68k_is_wb) {
        m68k_set_reg(8, 0);              /* A0 = NULL */
        m68k_set_reg(0, 0);              /* D0 = 0    */
    }

    /* Run in time-sliced chunks.
     * Liveness watchdog (UAOS-247): instead of a cumulative cycle budget —
     * which killed any interactive app after ~14 s — count only cycles
     * burned *without ever blocking*.  The glue bumps g_m68k_block_marks
     * every time the guest naps in Wait/WaitPort/WaitIO/WaitTOF, so an app
     * parked in an IDCMP loop stays at spin_cycles == 0 and runs for hours,
     * while a task that truly spins CPU-only crosses the budget and gets a
     * one-shot PC-ring dump.  The dump is diagnostic only: the timer ISR
     * still preempts a spinning guest, so the host never locks up. */
    g_emu_halted = 0;
    task->m68k_halted = 0;
    task->m68k_budget_dumped = 0;
    {
        /* Full wild-PC/instr-hook state reset — includes saw_app_code,
         * low_reentry, wild_abort and the hook's prev edge.  Without this
         * a second launch inherits stale flags and the decruncher's own
         * 0x2xxxx code PCs look like wild re-entries. */
        extern void m68k_reset_wild_state(void);
        m68k_reset_wild_state();
    }
    task->m68k_entry = entry;
    task->m68k_stack_top = sp;
    const uint64_t cycle_budget = 100000000ULL;  /* 100M *unblocked* cycles (~14s at 7MHz) */
    uint64_t spin_cycles = 0;                  /* cycles since the guest last blocked */
    extern volatile uint32_t g_m68k_block_marks;
    uint32_t marks_seen = g_m68k_block_marks;

    /* DEBUG watch: the loaded hunk blocks [0x20000, watch_hi) are the
     * decruncher's *input* — nothing in the guest should write them once
     * running.  Checksum each 4KB page per exec chunk; a divergence means
     * a host-side path (ISR/compositor/DMA) wrote into this task's window
     * while it was current.  Report the live Musashi PC at detection. */
#define WATCH_LO 0x00020000u
#define WATCH_HI 0x00080000u
#define WATCH_PG 0x00001000u
#define WATCH_NP ((WATCH_HI - WATCH_LO) / WATCH_PG)
    static uint32_t watch_sum[WATCH_NP];
    static uint8_t  watch_hit[WATCH_NP];
    static uint8_t watch_ref_buf[WATCH_HI - WATCH_LO];
    static uint8_t *watch_ref = watch_ref_buf;
    if (g_watch_mem) {
        extern void kprint(const char *);
        for (uint32_t p = 0; p < WATCH_NP; p++) {
            const uint32_t *w = (const uint32_t *)(g_ram + WATCH_LO + p * WATCH_PG);
            uint32_t s = 0;
            for (int i = 0; i < (int)(WATCH_PG / 4); i++) s += w[i];
            watch_sum[p] = s; watch_hit[p] = 0;
            if (watch_ref)
                __builtin_memcpy(watch_ref + p * WATCH_PG, w, WATCH_PG);
        }
        kprint("[watch] armed 0x20000-0x80000\n");
    }

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
        Chiptrace_PcSample();
        uint32_t ran = m68k_cycles_run();
        g_m68k_cycles += (uint64_t)ran;

        /* Liveness: any blocking nap during this slice proves the guest
         * can still be descheduled — reset its spin budget. */
        if (g_m68k_block_marks != marks_seen) {
            marks_seen = g_m68k_block_marks;
            spin_cycles = 0;
        } else {
            spin_cycles += ran;
        }

        /* Deliver pending guest interrupts (VERTB, AUDx, CIA) to the exec
         * Interrupt structures in this task's ExecBase.  The real
         * m68k_set_irq autovector path also fires at the next execute()
         * boundary — this poll covers the case where the guest never
         * drops IPL below 7. */
        { extern void UAOS_M68k_DeliverInterrupts(void); UAOS_M68k_DeliverInterrupts(); }
        /* Note: chip_emu_run_to_cycle() is NOT called here because the
         * chipset emulator uses global blitter/copper state that is
         * shared across all tasks.  Driving it from a per-task M68k
         * loop causes the blitter to access the wrong g_ram (this
         * task's guest RAM) when processing operations queued by
         * other tasks or the Workbench.  The chipset emulator is
         * driven by the global timer ISR instead. */

        /* DEBUG watch: detect external writes into the packed-hunk region */
        for (uint32_t p = 0; g_watch_mem && p < WATCH_NP; p++) {
            const uint32_t *w = (const uint32_t *)(g_ram + WATCH_LO + p * WATCH_PG);
            uint32_t s = 0;
            for (int i = 0; i < (int)(WATCH_PG / 4); i++) s += w[i];
            if (s != watch_sum[p] && !watch_hit[p]) {
                watch_hit[p] = 1;
                extern void kprint(const char *);
                extern unsigned int m68k_get_reg(void *ctx, int reg);
                extern volatile int g_irq_depth;
                /* Find the first byte that differs from the armed snapshot */
                int first = 0;
                if (watch_ref) {
                    const uint8_t *rp = watch_ref + p * WATCH_PG;
                    const uint8_t *cp = g_ram + WATCH_LO + p * WATCH_PG;
                    while (first < (int)WATCH_PG && rp[first] == cp[first]) first++;
                }
                char wb[200]; int wj = 0;
                const char *wl = "[watch] page +0x";
                while (wl[wj]) { wb[wj] = wl[wj]; wj++; }
                static const char hxw[] = "0123456789ABCDEF";
                uint32_t pa = WATCH_LO + p * WATCH_PG;
                for (int b = 7; b >= 0; b--) wb[wj++] = hxw[(pa >> (b*4)) & 15];
                const char *wm = " off=0x";
                for (int i = 0; wm[i]; i++) wb[wj++] = wm[i];
                for (int b = 2; b >= 0; b--) wb[wj++] = hxw[(first >> (b*4)) & 15];
                const char *mp = " pc=0x";
                for (int i = 0; mp[i]; i++) wb[wj++] = mp[i];
                uint32_t pcv = (uint32_t)m68k_get_reg(NULL, 16 /*M68K_REG_PC*/);
                for (int b = 7; b >= 0; b--) wb[wj++] = hxw[(pcv >> (b*4)) & 15];
                const char *mi = " irq=";
                for (int i = 0; mi[i]; i++) wb[wj++] = mi[i];
                wb[wj++] = hxw[g_irq_depth & 15];
                const char *md = " new=";
                for (int i = 0; md[i]; i++) wb[wj++] = md[i];
                for (int i = 0; i < 24 && wj < 170; i++) {
                    uint8_t bv = g_ram[pa + (uint32_t)first + (uint32_t)i];
                    wb[wj++] = hxw[(bv >> 4) & 15];
                    wb[wj++] = hxw[bv & 15];
                }
                wb[wj] = 0;
                kprint(wb); kprint("\n");
            }
            watch_sum[p] = s;
        }

        /* Check if the binary called Exit */
        if (g_emu_halted) {
            task->m68k_halted = 1;
            break;
        }

        /* Spin watchdog: only cycles burned without ever blocking count
         * (UAOS-247) — a guest parked in Wait/WaitPort resets spin_cycles
         * every slice, so interactive apps never trigger this.  Keep one
         * diagnostic PC-ring dump at the first crossing for hang analysis,
         * but let the task continue — the timer ISR still preempts it, so
         * the host shell stays responsive. */
        if (spin_cycles >= cycle_budget && !task->m68k_budget_dumped) {
            extern void kprint(const char *);
            extern uint32_t g_m68k_pc_ring[];
            extern int g_m68k_pc_ring_idx;
            extern uint32_t g_m68k_first_wild_pc;
            extern uint32_t g_m68k_first_wild_prev;
#define M68K_PC_RING_SZ_DUMP 256
            task->m68k_budget_dumped = 1;
            spin_cycles = 0;
            kprint("[m68k] spin watchdog fired (100M unblocked cycles) — dumping PCs, continuing\n");
            kprint("[m68k] last-PCs (newest first, 8/line):\n");
            {
                char wb[56]; int wj = 0;
                uint32_t wp = g_m68k_first_wild_pc;
                uint32_t wprev = g_m68k_first_wild_prev;
                static const char hx2[] = "0123456789ABCDEF";
                const char *wl = "[m68k] first wild PC: 0x";
                while (wl[wj]) { wb[wj] = wl[wj]; wj++; }
                for (int b = 7; b >= 0; b--) wb[wj++] = hx2[(wp >> (b*4)) & 15];
                const char *wl2 = " from 0x";
                for (int q = 0; wl2[q]; q++) wb[wj++] = wl2[q];
                for (int b = 7; b >= 0; b--) wb[wj++] = hx2[(wprev >> (b*4)) & 15];
                wb[wj] = 0;
                kprint(wb); kprint("\n");
            }
            for (int k = 0; k < M68K_PC_RING_SZ_DUMP; k += 8) {
                char rb[96]; int t = 0;
                static const char hx[] = "0123456789ABCDEF";
                for (int e = 0; e < 8; e++) {
                    uint32_t p = g_m68k_pc_ring[(g_m68k_pc_ring_idx - 1 - k - e) & 255];
                    if (e) rb[t++] = ' ';
                    rb[t++]='0'; rb[t++]='x';
                    for (int b = 7; b >= 0; b--) rb[t++] = hx[(p >> (b*4)) & 15];
                }
                rb[t] = 0;
                kprint(rb); kprint("\n");
            }
        }

        /* Diagnostic: print while the guest is continuously CPU-bound —
         * a blocked guest accumulates nothing, so this stays quiet for
         * interactive apps (UAOS-247). */
        if ((spin_cycles & 0x3FFFFFF) == 0 && spin_cycles > 0) {
            extern void kprint(const char *);
            kprint("[m68k] still running (64M+ unblocked cycles)\n");
        }

        /* Yield to other tasks — the timer ISR preempts us during
         * m68k_execute() and round-robins to the shell/idle tasks. */
        __asm__ volatile ("pause");
    }

    /* Re-enable chipset sync before exiting */
    extern int g_chipset_sync_disabled;
    g_chipset_sync_disabled = 0;

    /* Workbench launch teardown (UAOS-253): the WBStartup reply arrives
     * on the fake Workbench port via ReplyMsg — check it so unreplied
     * startups surface in the log.  The loaded seglist blocks were
     * allocated from this task's guest RAM window; freeing the window
     * (Task_ReleaseM68kRam via Task_Exit) releases them, and the guest
     * heap is torn down wholesale — nothing more to do. */
    if (task->m68k_is_wb) {
        extern void kprint(const char *);
        kprint(UAOS_Emu_WbStartupReplied()
               ? "[wb] startup message replied — process clean exit\n"
               : "[wb] WARN: program exited without ReplyMsg(WBStartup)\n");
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
    /* Snapshot the launcher-selected cwd (g_uaos_cwd) into the task so
     * later launches and other guests' cd calls can't rewire this task's
     * relative paths (dos_lib path resolution reads task_cwd via
     * m68k_cur_cwd). */
    {
        extern char g_uaos_cwd[64];
        int i = 0;
        while (i < (int)sizeof(t->task_cwd) - 1 && g_uaos_cwd[i]) {
            t->task_cwd[i] = g_uaos_cwd[i];
            i++;
        }
        t->task_cwd[i] = '\0';
    }
    t->m68k_ram = g_ram_pool[slot];
    t->m68k_context_size = ctx_size;
    t->m68k_bin_size = bin_size;
    t->m68k_context_buf = ctx_buf;
    t->native_arg = t;                /* pass task pointer to wrapper */
    t->m68k_print_fn = (void *)print_fn;

    /* Consume any pending Workbench launch descriptor (UAOS-253).  The
     * launcher fills g_wb_pending right before calling us; snapshot it
     * and clear so the next launch defaults back to CLI semantics. */
    t->m68k_wb = g_wb_pending;
    t->m68k_is_wb = g_wb_pending.used;
    g_wb_pending.used = 0;

    /* Copy the binary payload into the tail of the task's guest RAM NOW,
     * before the task starts running.  The caller's static buffer
     * (g_bin_payload) may be overwritten by other tasks or by the guest
     * RAM clear loop in m68k_wrapper_entry.  By copying here while we
     * still have the original pointer, we guarantee the data survives.
     * The wrapper task will read from this saved copy. */
    {
        uint32_t save_off = GUEST_RAM_SIZE - bin_size;
        save_off &= ~0xFu;  /* 16-byte align */
        t->m68k_bin_save_off = save_off;
        uint8_t *dst = g_ram_pool[slot] + save_off;
        for (uint32_t i = 0; i < bin_size; i++)
            dst[i] = binary[i];
    }

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

    uint64_t fl = irq_save();
    if (task->tc_State == TASK_READY) {
        /* Task is queued: move it to the new priority's ready list so
         * the change takes effect at the next dispatch instead of
         * silently keeping the old queue position. */
        ready_remove(task);
        task->ln_Pri = (int8_t)newpri;
        ready_enqueue(task);
    } else {
        task->ln_Pri = (int8_t)newpri;
    }
    irq_restore(fl);
}
