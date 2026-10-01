/* task.c — UAOS Unified Task Scheduler
 *
 * Priority-based round-robin for native x86_64 tasks.
 * Phase 1: native tasks only; M68k task fields are reserved.
 */

#include "task.h"
#include "syscall_table.h"
#include "../boot/kprint.h"
#include "../irq/ps2mouse.h"
#include "../irq/ps2kbd.h"
#include "../irq/irq.h"
#include "../display/framebuffer.h"
#include "../display/desktop.h"
#include "../display/cursor.h"
#include "../display/wm.h"
#include "../net/stack.h"
#include "../net/usock.h"
#include "../display/shell_win.h"
#include "../display/blanker.h"
#include "intuition_lib.h"
#include "memcheck.h"
#include <stdint.h>
#include <stddef.h>

extern volatile uint64_t g_pit_ticks;

/* Musashi M68k context save/restore (for switching between M68k tasks) */
extern unsigned int m68k_get_context(void *dst);
extern void m68k_set_context(void *src);
extern void m68k_end_timeslice(void);
extern int m68ki_initial_cycles;
extern int m68ki_remaining_cycles;
extern uint8_t *g_ram;

/* -------------------------------------------------------------------------
 * Globals
 * ------------------------------------------------------------------------- */

UaosTask g_tasks[MAX_TASKS];
uint8_t __attribute__((aligned(8))) g_task_stacks[MAX_TASKS][TASK_STACK_SIZE];
int      g_task_count = 0;
int32_t  g_task_bg_job = 0;
static UaosTask *g_current = NULL;
UaosTask *Task_SwitchNext = NULL;
UaosTask *Task_SwitchPrev = NULL;

/* Deferred-reschedule flag — the SysFlags SF_SAR analogue.  Set when an
 * IRQ-level do_schedule() is suppressed by Forbid/Disable nesting; the
 * request would otherwise be dropped and the newly-readied task would
 * wait for the next unsuppressed tick.  Permit()/Enable() honour it via
 * Task_CheckResched() when the nesting count unwinds to zero. */
static volatile int g_need_resched = 0;

/* Set by do_schedule(0) once a syscall-side switch is committed — from
 * just before g_current changes until the uaos_syscall_isr epilogue has
 * loaded the new task's RSP.  The int 0x80 gate is a TRAP gate (IF stays
 * set), so an IRQ can nest inside that window; the nested isr_common's
 * do_schedule(1)/epilogue must not touch the armed switch — g_current
 * already names the incoming task, so the nested epilogue would file
 * the physical RSP (old task's stack) into the WRONG task's native_rsp,
 * and resuming that task later iretqs a garbage frame (UAOS-180:
 * #GP at the syscall ISR's iretq on MBP4,1, EventPump victim). */
volatile int g_sched_switch_pending = 0;

/* The EventPump service task — registered by Task_EventPumpEntry so IRQ
 * and task-context producers can Signal() it via EventPump_Wake(). */
static UaosTask * volatile g_eventpump_task = NULL;

/* g_wait_tof_task defined in graphics_lib.c (extern in task.h) */

/* Ready queues: one doubly-linked list per priority level, plus a
 * 256-bit occupancy bitmap so dispatch finds the highest non-empty
 * queue without scanning all 256 lists. */
static UaosTask g_ready_heads[256];  /* index 0 = pri -128 */
static uint64_t g_ready_map[4];      /* bit i set if ready queue i is non-empty */

/* Wait queue — tasks blocked on Wait() */
static UaosTask g_wait_head;
static int      g_wait_count = 0;

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static inline void list_init(UaosTask *node)
{
    node->ln_Succ = node;
    node->ln_Pred = node;
}

static inline int list_empty(UaosTask *node)
{
    return node->ln_Succ == node;
}

static inline void list_remove(UaosTask *node)
{
    node->ln_Pred->ln_Succ = node->ln_Succ;
    node->ln_Succ->ln_Pred = node->ln_Pred;
    node->ln_Pred = node;
    node->ln_Succ = node;
}

static inline void list_append(UaosTask *head, UaosTask *node)
{
    node->ln_Pred = head->ln_Pred;
    node->ln_Succ = head;
    head->ln_Pred->ln_Succ = node;
    head->ln_Pred = node;
}

static inline void list_remove_head(UaosTask *head, UaosTask **out)
{
    *out = head->ln_Succ;
    if (*out != head)
        list_remove(*out);
    else
        *out = NULL;
}

static inline int pri_to_idx(int8_t pri)
{
    return (int)pri + 128;   /* -128..127 -> 0..255 */
}

/* -------------------------------------------------------------------------
 * Ready queue management
 * ------------------------------------------------------------------------- */

void ready_enqueue(UaosTask *task)
{
    int idx = pri_to_idx(task->ln_Pri);
    list_append(&g_ready_heads[idx], task);
    g_ready_map[idx >> 6] |= 1ULL << (idx & 63);
    task->tc_State = TASK_READY;
}

/* Remove a task from its ready queue (e.g. SetTaskPri reprioritising).
 * Caller must guarantee the task is currently queued. */
void ready_remove(UaosTask *task)
{
    int idx = pri_to_idx(task->ln_Pri);
    uint64_t bit = 1ULL << (idx & 63);
    list_remove(task);
    /* Clear-then-recheck: safe against an IRQ that enqueues to the same
     * queue between the list op and the bitmap op (ready_enqueue sets
     * the bit after appending, so either side restores it). */
    g_ready_map[idx >> 6] &= ~bit;
    if (!list_empty(&g_ready_heads[idx]))
        g_ready_map[idx >> 6] |= bit;
}

static UaosTask *ready_dequeue_highest(void)
{
    /* Highest set bitmap bit = highest non-empty priority queue. */
    for (int w = 3; w >= 0; w--) {
        while (g_ready_map[w]) {
            uint64_t bits = g_ready_map[w];
            int idx = (w << 6) + (63 - __builtin_clzll(bits));
            UaosTask *t;
            list_remove_head(&g_ready_heads[idx], &t);
            /* Same clear-then-recheck as ready_remove: an IRQ-level
             * enqueue landing mid-sequence leaves the bit set, matching
             * the now non-empty queue. */
            g_ready_map[w] &= ~(1ULL << (idx & 63));
            if (!list_empty(&g_ready_heads[idx]))
                g_ready_map[w] |= 1ULL << (idx & 63);
            if (t->tc_State != TASK_REMOVED)
                return t;
            /* Marked REMOVED while still queued (e.g. RemTask): drop it. */
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Wait queue management
 * ------------------------------------------------------------------------- */

static void wait_enqueue(UaosTask *task)
{
    list_append(&g_wait_head, task);
    g_wait_count++;
    task->tc_State = TASK_WAITING;
}

void wait_remove(UaosTask *task)
{
    list_remove(task);
    if (g_wait_count > 0) g_wait_count--;
}

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */
static void copy_cwd(char *dst, const char *src)
{
    int i = 0;
    if (src) {
        while (i < 127 && src[i]) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

/* -------------------------------------------------------------------------
 * Task creation
 * ------------------------------------------------------------------------- */

UaosTask *Task_CreateNative(const char *name, int8_t pri,
                            void (*entry)(void *), void *arg)
{
    Forbid();

    if (g_task_count >= MAX_TASKS) { Permit(); return NULL; }

    UaosTask *t = &g_tasks[g_task_count];
    uint8_t *stack = g_task_stacks[g_task_count];
    g_task_count++;

    /* Zero struct */
    for (int i = 0; i < (int)sizeof(UaosTask); i++)
        ((uint8_t *)t)[i] = 0;

    t->ln_Type = 1;  /* NT_TASK */
    t->ln_Pri = pri;
    t->ln_Name = name;
    list_init(t);

    t->tc_Flags = 0;
    t->tc_State = TASK_READY;
    t->tc_IDNestCnt = 0;
    t->tc_TDNestCnt = 0;
    t->tc_SigAlloc = 0xFFFF;
    t->tc_SigWait = 0;
    t->tc_SigRecvd = 0;
    t->tc_SigExcept = 0;
    t->tc_SPLower = stack;
    t->tc_SPUpper = stack + TASK_STACK_SIZE;

    t->type = TASK_TYPE_NATIVE;
    t->native_stack_base = stack;
    t->native_stack_size = TASK_STACK_SIZE;
    t->native_entry = entry;
    t->native_arg = arg;
    t->parent = Task_Current();
    t->bg_job = g_task_bg_job;
    copy_cwd(t->task_cwd, "");

    /* Build initial stack frame that looks like what the timer ISR pushes:
     *
     * Top of stack (highest address):
     *   [SS]     <- only for ring transitions (we are always ring 0, so omitted)
     *   [RSP_prev]
     *   [RFLAGS]
     *   [CS]
     *   [RIP]  = entry function
     *   [error_code] (0)
     *   [vector]     (0)
     *   [R15..RAX]   (zeroed)
     *   [ret_addr]   (to isr_common epilogue)  <-- actually we will iretq directly
     *
     * Because we are always ring 0, the CPU pushes RFLAGS, CS, RIP only.
     * Our isr_common pushes R15..RAX, then vector, error_code.
     * We want to "return" to the new task via the same path.
     */

    uint64_t *sp = (uint64_t *)(stack + TASK_STACK_SIZE);

    /* Build synthetic interrupt frame at the top of the stack.
     * isr_common pushes 15 GPRs (RAX .. R15) AFTER the stub pushes
     * error_code and vector.  The CPU already pushed RIP, CS, RFLAGS.
     *
     * Memory layout (ascending addresses from native_rsp):
     *   sp[0]  = R15  (popped first)
     *   sp[1]  = R14
     *   ...
     *   sp[14] = RAX  (popped last)
     *   sp[15] = vector
     *   sp[16] = error_code
     *   sp[17] = RIP
     *   sp[18] = CS
     *   sp[19] = RFLAGS
     *   sp[20] = RSP
     *   sp[21] = SS
     */
    sp -= 22;
    for (int i = 0; i < 15; i++) sp[i] = 0;
    sp[9] = (uint64_t)arg;                      /* RDI — first argument */
    sp[15] = 0;                                 /* vector */
    sp[16] = 0;                                 /* error_code */
    sp[17] = (uint64_t)entry;                   /* RIP */
    sp[18] = 0x08;                              /* CS */
    sp[19] = 0x202;                             /* RFLAGS: IF=1 */
    sp[20] = (uint64_t)(stack + TASK_STACK_SIZE); /* RSP (for iretq safety) */
    sp[21] = 0x10;                              /* SS  (kernel data seg) */

    t->native_rsp = (uint64_t)sp;  /* points to R15 slot */

    ready_enqueue(t);
    Permit();
    return t;
}

/* -------------------------------------------------------------------------
 * Create an x86-64 task from an ELF64-loaded image
 * ------------------------------------------------------------------------- */

/* Assembly trampoline used by Task_RunNew for the very first switch to
 * an ELF64 task.  It switches to the ELF64 user stack and jumps to the
 * loaded entry point.  The argument is the task pointer. */
extern void Task_RunNewX64(void *task);

UaosTask *Task_CreateX64(const char *name, int8_t pri,
                         uint64_t entry_rip, uint64_t initial_rsp,
                         const char *cwd,
                         void (*print_fn)(void *ctx, const char *line),
                         void *print_ctx)
{
    Forbid();

    /* Find a free task slot — prefer reusing REMOVED tasks, then append. */
    int slot = -1;
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].tc_State == TASK_REMOVED) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (g_task_count >= MAX_TASKS) { Permit(); return NULL; }
        slot = g_task_count;
        g_task_count++;
    }

    UaosTask *t = &g_tasks[slot];
    uint8_t *stack = g_task_stacks[slot];

    for (int i = 0; i < (int)sizeof(UaosTask); i++)
        ((uint8_t *)t)[i] = 0;

    t->ln_Type = 1;  /* NT_TASK */
    t->ln_Pri = pri;
    t->ln_Name = name;
    list_init(t);

    t->tc_Flags = 0;
    t->tc_State = TASK_READY;
    t->tc_IDNestCnt = 0;
    t->tc_TDNestCnt = 0;
    t->tc_SigAlloc = 0xFFFF;
    t->tc_SigWait = 0;
    t->tc_SigRecvd = 0;
    t->tc_SigExcept = 0;
    t->tc_SPLower = stack;
    t->tc_SPUpper = stack + TASK_STACK_SIZE;

    t->type = TASK_TYPE_X64;
    t->native_stack_base = stack;
    t->native_stack_size = TASK_STACK_SIZE;
    t->native_entry = Task_RunNewX64;
    t->native_arg = t;
    t->native_rip = entry_rip;
    t->native_initial_rsp = initial_rsp;
    t->parent = Task_Current();
    t->bg_job = g_task_bg_job;
    copy_cwd(t->task_cwd, cwd);
    t->native_print_fn  = print_fn;
    t->native_print_ctx = print_ctx;
    t->task_out_len     = 0;

    /* Build synthetic interrupt frame on the kernel task stack.
     * When Task_SwitchContext restores this frame and executes iretq,
     * the CPU resumes at the ELF64 entry point with the user stack. */
    uint64_t *sp = (uint64_t *)(stack + TASK_STACK_SIZE);
    sp -= 22;
    for (int i = 0; i < 15; i++) sp[i] = 0;
    sp[15] = 0;                                 /* vector */
    sp[16] = 0;                                 /* error_code */
    sp[17] = entry_rip;                         /* RIP */
    sp[18] = 0x08;                              /* CS  — kernel code (ring 0)   */
    sp[19] = 0x202;                             /* RFLAGS: IF=1 */
    sp[20] = initial_rsp;                       /* RSP (user stack)            */
    sp[21] = 0x10;                              /* SS  — kernel data (ring 0)   */

    t->native_rsp = (uint64_t)sp;  /* kernel stack frame pointer */

    ready_enqueue(t);
    Permit();
    return t;
}

/* -------------------------------------------------------------------------
 * Scheduling
 * ------------------------------------------------------------------------- */

/* Synchronous reschedule from task context (UAOS-169).
 *
 * Enters the INT 0x80 syscall ISR so the CPU and stub prologue build a
 * full saved frame — identical to an IRQ frame — on the current stack.
 * do_schedule(0) then arms Task_SwitchNext and the ISR epilogue performs
 * the context switch immediately; the caller resumes here only after it
 * has been re-dispatched.  This is what lets a blocking task hand the
 * CPU to the next ready task *now* instead of idling in `sti; hlt` as
 * g_current until the next PIT tick.
 *
 * `int` is a trap, not a maskable interrupt, so it works with IF=0 (all
 * the blocking primitives call it from their cli region); the vector
 * 0x80 gate is a trap gate, preserving whatever IF the caller had.
 *
 * Returns nonzero when a switch to a different task was armed, zero when
 * nothing else was runnable and no switch happened (Syscall_Dispatch
 * reports the SYSCALL_SCHEDULE result).  Callers use the zero case to
 * fall back to hlt so the CPU still sleeps rather than spinning on the
 * trap when the ready queues are empty. */
static int task_switch_away(void)
{
    uint64_t ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"((uint64_t)SYSCALL_SCHEDULE)
                      : "memory", "cc");
    return (int)ret;
}

static void do_schedule(int from_irq)
{
    if (!g_current) return;

    /* A syscall-side switch is armed but not yet consumed — defer (see
     * g_sched_switch_pending above).  Applies to both paths: an IRQ may
     * nest in the trap-gate window, and a nested int $0x80 issued before
     * the outer epilogue would see g_current already pointing at the
     * incoming task and misfile the physical RSP just the same. */
    if (g_sched_switch_pending) {
        g_need_resched = 1;
        return;
    }

    /* An IRQ-side switch is already armed for this interrupt exit.  One
     * ISR can reach here several times (PIT → USB_Poll → HID callback →
     * EventPump_Wake, then the PIT handler's own schedule; or two USB
     * callbacks in one UHCI scan).  Re-running would treat the incoming
     * task as prev, re-enqueue it and overwrite Task_SwitchPrev, so the
     * epilogue files the outgoing RSP into the wrong native_rsp and the
     * real outgoing task later resumes a stale frame (UAOS-181: #GP at
     * the syscall iretq on MBP4,1, task bcm5974-reset).  The armed switch
     * stands; anything readied since is picked up at the next tick. */
    if (Task_SwitchNext)
        return;

    if (from_irq) {
        /* Honour Forbid / Disable nesting — timer ISR only.  Record the
         * suppressed reschedule so Permit()/Enable() can dispatch it
         * when the nesting unwinds instead of dropping it outright. */
        if (g_current->tc_TDNestCnt > 0 || g_current->tc_IDNestCnt > 0) {
            g_need_resched = 1;
            return;
        }
    }
    g_need_resched = 0;

    UaosTask *prev = g_current;
    if (prev->tc_State == TASK_RUNNING) {
        prev->tc_State = TASK_READY;
        ready_enqueue(prev);
    }

    UaosTask *next = ready_dequeue_highest();
    if (!next) return;   /* nothing else to run */

    if (next == prev) {
        /* We were the only runnable task; keep running */
        g_current = prev;
        prev->tc_State = TASK_RUNNING;
        return;
    }

    /* If the current task is an M68k guest, stop Musashi and save context */
    if (prev->type == TASK_TYPE_M68K) {
        m68k_end_timeslice();
        if (prev->m68k_context_buf) {
            m68k_get_context(prev->m68k_context_buf);
            prev->m68k_initial_cycles = m68ki_initial_cycles;
            prev->m68k_remaining_cycles = m68ki_remaining_cycles;
        }
    }

    /* Up to here a nested IRQ still sees physical context == g_current,
     * so it may park/switch correctly.  Past this line g_current names
     * the incoming task while the CPU is still on the outgoing stack —
     * raise the pending flag so a nested IRQ defers instead of misfiling
     * this RSP into `next`'s native_rsp. */
    if (!from_irq) g_sched_switch_pending = 1;

    g_current = next;
    next->tc_State = TASK_RUNNING;

    /* If the new task is an M68k guest, restore its context and RAM */
    if (next->type == TASK_TYPE_M68K) {
        if (next->m68k_context_buf) {
            m68k_set_context(next->m68k_context_buf);
            m68ki_initial_cycles = next->m68k_initial_cycles;
            m68ki_remaining_cycles = next->m68k_remaining_cycles;
        }
        g_ram = next->m68k_ram;
    }

    /* Tell isr_common to perform the switch */
    Task_SwitchPrev = prev;
    Task_SwitchNext = next;

}

void Task_ScheduleFromIRQ(void)
{
    do_schedule(1);
}

void Task_ScheduleFromSyscall(void)
{
    do_schedule(0);
}

int Task_Yield(void)
{
    /* Voluntary reschedule through the syscall ISR (UAOS-169): the
     * switch rides a real saved frame, so do_schedule(0) runs now rather
     * than at the next PIT tick.  Stays a no-op in IRQ context and under
     * Forbid/Disable — a voluntary yield must not break a caller's
     * critical section; code that wants to sleep there should call
     * Wait()/Task_SleepTicks(), which always deschedule. */
    if (!g_current || g_irq_depth > 0)
        return 0;
    if (g_current->tc_TDNestCnt > 0 || g_current->tc_IDNestCnt > 0)
        return 0;
    return task_switch_away();
}

void Task_Exit(void)
{
    /* Mark task as removed so the scheduler never picks it again.
     * Signal the parent so a shell (or other task) waiting via SIGF_CHILD
     * knows the foreground command has finished. */
    if (g_current) {
        /* If this task is the one blocked in WaitTOF(), clear the global. */
        if (g_wait_tof_task == g_current) g_wait_tof_task = NULL;
        /* Free the M68k VBlank signal bit if one was allocated. */
        if (g_current->type == TASK_TYPE_M68K && g_current->m68k_vblank_sig >= 0) {
            g_current->tc_SigAlloc |= (1u << (unsigned int)g_current->m68k_vblank_sig);
            g_current->m68k_vblank_sig = -1;
        }
        /* Reclaim the M68k task's tracked guest AllocMem blocks while
         * g_ram still maps its address space (cycle-budget aborts and
         * plain Exit() both end here). */
        if (g_current->type == TASK_TYPE_M68K) {
            extern int g_chipset_sync_disabled;
            g_chipset_sync_disabled = 0;   /* may bypass wrapper cleanup */
            Memcheck_FreeByOwner(g_current);
        }
        /* Release M68k guest RAM so the slot can be reused. */
        Task_ReleaseM68kRam(g_current);
        /* Close any userspace sockets this task left open so the
         * usock/tcp slots are not leaked by a killed command. */
        usock_cleanup_task(g_current);
        /* Drop the task's net RX-notify slot so a stale TCB pointer can
         * never be signalled by a later net_rx_kick(). */
        net_rx_notify_disarm(g_current);
        g_current->tc_State = TASK_REMOVED;
        if (g_current->parent && g_current->parent->tc_State != TASK_REMOVED)
            Signal(g_current->parent, SIGF_CHILD);
        /* Reclaim this X64 task's heap blocks individually, then reset
         * the arena entirely if no X64 tasks remain. */
        if (g_current->type == TASK_TYPE_X64) {
            extern void ELF64_FreeTaskBlocks(void *task);
            extern void ELF64_ReclaimHeap(void);
            ELF64_FreeTaskBlocks(g_current);
            ELF64_ReclaimHeap();
        }
    }
    /* Switch away immediately (UAOS-169): a REMOVED task is never
     * re-dispatched, so a successful switch never returns here.  If
     * nothing else was runnable the int returns 0 — hlt then sleeps
     * until the next IRQ retries the schedule instead of spinning. */
    for (;;) {
        if (!task_switch_away())
            __asm__ volatile ("sti; hlt" ::: "memory");
    }
}

UaosTask *Task_Current(void)
{
    return g_current;
}

/* -------------------------------------------------------------------------
 * Scheduler init
 * ------------------------------------------------------------------------- */

void TaskScheduler_Init(void)
{
    g_task_count = 0;
    g_current = NULL;
    for (int i = 0; i < 256; i++)
        list_init(&g_ready_heads[i]);
    for (int i = 0; i < 4; i++)
        g_ready_map[i] = 0;
    list_init(&g_wait_head);
    g_wait_count = 0;

    kprint("[TASK] Scheduler initialised\n");
}

void Task_StartFirst(void)
{
    UaosTask *first = ready_dequeue_highest();
    if (first) {
        g_current = first;
        first->tc_State = TASK_RUNNING;
        extern void Task_RunNew(UaosTask *);
        Task_RunNew(first);
    }
    kprint("[TASK] No ready tasks - halting\n");
    /* No tasks - halt */
    for (;;) __asm__ volatile ("cli; hlt");
}

/* -------------------------------------------------------------------------
 * Idle task — lowest priority, halts the CPU when nothing else is ready
 * ------------------------------------------------------------------------- */
void Task_IdleEntry(void *arg)
{
    (void)arg;
    for (;;) __asm__ volatile ("hlt" ::: "memory");
}

/* -------------------------------------------------------------------------
 * Event pump — WM/input/network/job servicing (the former Idle body).
 *
 * Signal-driven: the pump blocks in Task_WaitTicks() on SIGF_EVENTPUMP
 * (input drivers, RTC second tick, WM damage, job enqueue), SIGF_NET
 * (armed via net_rx_notify_arm, kicked by NIC IRQs) and SIGF_CHILD
 * (pump-spawned tasks such as background jobs exiting).  A 100-tick
 * timeout is a 1 s safety net matching the menubar-clock cadence.
 *
 * Runs at handler priority (0), NOT idle priority: it must respond to
 * desktop input promptly even while other pri-0 work is queued, and at
 * -128 strict priority every-tick wakers would starve it forever —
 * which freezes the desktop while the rest of the system looks healthy.
 * ------------------------------------------------------------------------- */
void EventPump_Wake(void)
{
    UaosTask *t = g_eventpump_task;
    if (t && t->tc_State != TASK_REMOVED)
        Signal(t, SIGF_EVENTPUMP);
    /* From IRQ context also request a reschedule so the woken pump is
     * dispatched at interrupt exit instead of the next PIT tick.  In
     * task context this is left to the normal schedule points — calling
     * Task_ScheduleFromIRQ outside a saved ISR frame corrupts state. */
    if (g_irq_depth > 0)
        Task_ScheduleFromIRQ();
}

void Task_EventPumpEntry(void *arg)
{
    (void)arg;
    int last_mx = -1, last_my = -1, last_btn = -1, last_btn_right = -1;

    /* Register as the wake target before the first wait; producers that
     * signalled before this point are still picked up because their
     * state changes (key buffer, mouse position, damage flag) persist. */
    g_eventpump_task = Task_Current();
    net_rx_notify_arm();

    for (;;) {
        /* --- Protected section: WM + input + network + jobs --- */
        Forbid();

        /* Mouse -> WM */
        if (g_fb.valid) {
            int mx = g_mouse.x, my = g_mouse.y;
            int btn_left = g_mouse.btn_left;
            int btn_right = g_mouse.btn_right;
            if (mx != last_mx || my != last_my ||
                btn_left != last_btn || btn_right != last_btn_right) {
                last_mx = mx; last_my = my;
                last_btn = btn_left; last_btn_right = btn_right;
                Blanker_OnInput();
                WM_MouseEvent(mx, my, btn_left, btn_right);
            }
        }

        /* Keyboard -> WM (Amiga key combos first, then command-key
         * shortcuts, then regular keys) */
        while (PS2Kbd_HasChar()) {
            char c = PS2Kbd_GetChar();
            unsigned char uc = (unsigned char)c;
            Blanker_OnInput();

            /* LAmiga+M/N — screen cycling */
            if (uc == (unsigned char)AMIGA_LM) {
                UAOS_Intuition_CycleScreen(1);
                continue;
            }
            if (uc == (unsigned char)AMIGA_LN) {
                UAOS_Intuition_CycleScreen(-1);
                continue;
            }
            /* LAmiga+V/B — requester Verify/Cancel (future: route to
             * active requester).  For now, consume so they don't leak
             * into text input. */
            if (uc == (unsigned char)AMIGA_LV ||
                uc == (unsigned char)AMIGA_LB)
                continue;

            /* RAmiga+letter — menu shortcut via Intuition command key */
            if (IS_AMIGA_RKEY(c)) {
                char letter = AMIGA_RLETTER(c);
                if (!Intuition_InvokeCommandKey(letter))
                    WM_KeyEvent(c);
                continue;
            }

            /* Regular key — try command key first, then WM */
            if (!Intuition_InvokeCommandKey(c))
                WM_KeyEvent(c);
        }

        /* Clock redraw */
        Desktop_FlushClockRedraw();

        /* Network */
        net_stack_poll();

        /* Background jobs */
        if (!PS2Kbd_HasChar())
            ShellWin_PollJobs();

        /* Coalesced repaint: event handlers accumulate damage instead of
         * repainting per event — flush once per iteration (UAOS-101).
         * Then apply any IRQ-deferred cursor move (UAOS-104). */
        WM_FlushRedraw();
        Cursor_Flush();

        Permit();
        /* --- End protected section --- */

        /* Block until the next event instead of spinning on hlt: the
         * producers above Signal() us and IRQ-side wakes reschedule at
         * interrupt exit (or at Permit via g_need_resched).  Signals
         * that landed during the work body are latched in tc_SigRecvd,
         * so this returns immediately when there is pending input.
         * The timeout is only a safety net for unsignalled producers. */
        Task_WaitTicks(SIGF_EVENTPUMP | SIGF_NET | SIGF_CHILD, 100);
    }
}

/* -------------------------------------------------------------------------
 * Test tasks
 * ------------------------------------------------------------------------- */

static void test_task_a(void *arg)
{
    (void)arg;
    kprint("[TASK-A] START\n");
    for (;;) {
        kprint("[TASK-A] running\n");
        volatile uint64_t n = 5000000;
        while (n--) __asm__ volatile ("pause");
    }
}

static void test_task_b(void *arg)
{
    (void)arg;
    kprint("[TASK-B] START\n");
    for (;;) {
        kprint("[TASK-B] running\n");
        volatile uint64_t n = 5000000;
        while (n--) __asm__ volatile ("pause");
    }
}

static void test_task_c(void *arg)
{
    (void)arg;
    kprint("[TASK-C] START\n");
    for (;;) {
        kprint("[TASK-C] running\n");
        volatile uint64_t n = 5000000;
        while (n--) __asm__ volatile ("pause");
    }
}

void Task_TestSpawn(void)
{
    kprint("[TASK] Spawning test tasks...\n");
    Task_CreateNative("TestA", 0, test_task_a, NULL);
    Task_CreateNative("TestB", 0, test_task_b, NULL);
    Task_CreateNative("TestC", 0, test_task_c, NULL);
    kprint("[TASK] Test tasks spawned\n");
}

/* =========================================================================
 * Signal / Wait / Critical sections
 * ========================================================================= */

void Signal(UaosTask *task, uint32_t sigmask)
{
    if (!task || !sigmask) return;

    /* Save/restore IF instead of an unconditional cli/sti pair: with the
     * old code a Signal() from interrupt context would re-enable IRQs in
     * the middle of the ISR.  Restoring the caller's flags makes Signal
     * callable from both task and IRQ context (e.g. a NIC interrupt
     * waking a net consumer task directly). */
    uint64_t fl = irq_save();
    task->tc_SigRecvd |= sigmask;

    if (task->tc_State == TASK_WAITING && (task->tc_SigRecvd & task->tc_SigWait) != 0) {
        wait_remove(task);
        ready_enqueue(task);
    }
    irq_restore(fl);
}

uint32_t Wait(uint32_t sigmask)
{
    uint32_t result;

    /* Save/restore IF: a bare sti at exit would silently re-enable
     * interrupts if the caller entered with IF=0 (Disable() nesting or
     * an outer irq_save region) — see UAOS-176. */
    uint64_t fl = irq_save();
    g_current->tc_SigWait = sigmask;

    while ((g_current->tc_SigRecvd & sigmask) == 0) {
        /* Enqueue only once: a non-PIT interrupt (kbd/mouse/NIC) wakes
         * the hlt and returns here WITHOUT rescheduling, so re-enqueueing
         * a task still linked in the wait list corrupts it (the node is
         * appended twice; a later wait_remove can leave a succ self-loop
         * that hangs Task_WakeTimers inside the PIT ISR). */
        if (g_current->tc_State != TASK_WAITING) {
            g_current->tc_State = TASK_WAITING;
            wait_enqueue(g_current);
        }
        /* Switch to the next ready task immediately (UAOS-169):
         * task_switch_away() enters the syscall ISR (a trap — safe with
         * IF=0, unlike the sti;int $0x80 shadow this used to worry
         * about) so the ISR epilogue context-switches right now instead
         * of idling as g_current in sti;hlt until the next PIT tick.
         * When a matching Signal() re-readies us, a later schedule
         * resumes execution here.
         *
         * task_switch_away() returns 0 only when nothing else was
         * runnable; the hlt fallback then sleeps until the next IRQ
         * rather than spinning on the trap. */
        if (!task_switch_away())
            __asm__ volatile ("sti; hlt" ::: "memory");
        __asm__ volatile ("cli");
    }

    result = g_current->tc_SigRecvd & sigmask;
    g_current->tc_SigRecvd &= ~sigmask;
    g_current->tc_SigWait = 0;
    irq_restore(fl);

    return result;
}

void Task_SleepTicks(uint64_t ticks)
{
    if (!g_current || ticks == 0) return;
    uint64_t deadline = g_pit_ticks + ticks;

    uint64_t fl = irq_save();           /* UAOS-176: restore caller's IF */
    g_current->tc_SigWait = 0;          /* not woken by Signal() */
    g_current->tc_wake_tick = deadline;

    while (g_pit_ticks < deadline) {
        /* Enqueue only once — see Wait(): a non-PIT interrupt can wake
         * the hlt without rescheduling; re-enqueueing a task that is
         * still linked corrupts the wait list. */
        if (g_current->tc_State != TASK_WAITING) {
            g_current->tc_State = TASK_WAITING;
            wait_enqueue(g_current);
        }
        /* Same mechanism as Wait() (UAOS-169): switch to the next ready
         * task via the syscall-ISR epilogue now; Task_WakeTimers() moves
         * us back to the ready queue once the deadline passes and we
         * resume here when re-dispatched.  The hlt fallback covers the
         * nothing-else-runnable case so the CPU still sleeps. */
        if (!task_switch_away())
            __asm__ volatile ("sti; hlt" ::: "memory");
        __asm__ volatile ("cli");
    }

    g_current->tc_wake_tick = 0;
    irq_restore(fl);
}

uint32_t Task_WaitTicks(uint32_t sigmask, uint64_t ticks)
{
    if (!g_current) return 0;
    if (!sigmask) { Task_SleepTicks(ticks); return 0; }

    uint64_t deadline = g_pit_ticks + ticks;
    uint32_t result;

    uint64_t fl = irq_save();           /* UAOS-176: restore caller's IF */
    g_current->tc_SigWait  = sigmask;
    g_current->tc_wake_tick = deadline;

    /* Block until a matching signal arrives or the deadline passes.
     * Signal() from IRQ/task context moves us wait->ready; Task_WakeTimers
     * does the same on timeout.  Only enqueue once — a wake that doesn't
     * move us back to ready leaves us still linked (see Wait()).
     * task_switch_away() deschedules immediately (UAOS-169); hlt is the
     * nothing-else-runnable fallback. */
    while ((g_current->tc_SigRecvd & sigmask) == 0 &&
           g_pit_ticks < deadline) {
        if (g_current->tc_State != TASK_WAITING) {
            g_current->tc_State = TASK_WAITING;
            wait_enqueue(g_current);
        }
        if (!task_switch_away())
            __asm__ volatile ("sti; hlt" ::: "memory");
        __asm__ volatile ("cli");
    }

    result = g_current->tc_SigRecvd & sigmask;
    g_current->tc_SigRecvd &= ~sigmask;
    g_current->tc_SigWait  = 0;
    g_current->tc_wake_tick = 0;
    irq_restore(fl);

    return result;
}

void Task_WakeTimers(void)
{
    UaosTask *t = g_wait_head.ln_Succ;
    while (t != &g_wait_head) {
        UaosTask *next = t->ln_Succ;
        if (t->tc_State == TASK_REMOVED) {
            /* Dead task left linked (e.g. RemTask on a waiter): unlink. */
            wait_remove(t);
        } else if (t->tc_wake_tick && g_pit_ticks >= t->tc_wake_tick) {
            t->tc_wake_tick = 0;
            wait_remove(t);
            ready_enqueue(t);
        }
        t = next;
    }
}

void Task_ClearSig(uint32_t sigmask)
{
    uint64_t fl = irq_save();
    if (g_current)
        g_current->tc_SigRecvd &= ~sigmask;
    irq_restore(fl);
}

uint32_t SetSignal(uint32_t newsignals, uint32_t sigmask)
{
    uint64_t fl = irq_save();
    uint32_t old = g_current->tc_SigRecvd;
    g_current->tc_SigRecvd = (old & ~sigmask) | (newsignals & sigmask);
    irq_restore(fl);
    return old;
}

void Forbid(void)
{
    if (g_current) g_current->tc_TDNestCnt++;
}

/* Dispatch a reschedule that was deferred by g_need_resched.  The switch
 * can only happen on a saved interrupt frame, so we go through the
 * INT 0x80 syscall ISR (SYSCALL_SCHEDULE → Task_ScheduleFromSyscall →
 * do_schedule(0)); its epilogue performs the context switch and we resume
 * here when this task is re-dispatched. */
static void Task_CheckResched(void)
{
    if (!g_current || !g_need_resched) return;
    if (g_current->tc_TDNestCnt > 0 || g_current->tc_IDNestCnt > 0) return;
    if (g_irq_depth > 0) return;   /* called from inside an ISR body */

    uint64_t fl;
    __asm__ volatile ("pushfq; popq %0" : "=r"(fl));
    if (!(fl & 0x200)) return;     /* caller holds a raw cli region */

    g_need_resched = 0;
    uint64_t ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"((uint64_t)SYSCALL_SCHEDULE)
                      : "memory", "cc");
    (void)ret;
}

void Permit(void)
{
    if (!g_current) return;
    if (--g_current->tc_TDNestCnt <= 0) {
        g_current->tc_TDNestCnt = 0;
        /* Run a reschedule an IRQ deferred while we were forbidden. */
        Task_CheckResched();
    }
}

void Disable(void)
{
    __asm__ volatile ("cli");
    if (g_current) g_current->tc_IDNestCnt++;
}

void Enable(void)
{
    if (!g_current) {
        __asm__ volatile ("sti");
        return;
    }
    if (--g_current->tc_IDNestCnt <= 0) {
        g_current->tc_IDNestCnt = 0;
        __asm__ volatile ("sti");
        /* Interrupts are back on — a deferred reschedule can dispatch.
         * Task_CheckResched() no-ops in IRQ context (g_irq_depth > 0),
         * where Enable() is legal (e.g. virtio_net TX from the NIC ISR). */
        Task_CheckResched();
    }
}

/* =========================================================================
 * Scheduler state query (for status bar etc.)
 * ========================================================================= */

void Task_GetCounts(int *out_total, int *out_running, int *out_waiting)
{
    int total = 0, running = 0, waiting = 0;
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].tc_State == TASK_REMOVED) continue;
        total++;
        if (g_tasks[i].tc_State == TASK_RUNNING) running++;
        else if (g_tasks[i].tc_State == TASK_WAITING) waiting++;
    }
    if (out_total)   *out_total   = total;
    if (out_running) *out_running = running;
    if (out_waiting) *out_waiting = waiting;
}
