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
#include "../net/telnetd.h"
#include "../display/shell_win.h"
#include "../display/blanker.h"
#include "intuition_lib.h"
#include "memcheck.h"
#include "../dos/handle_table.h"
#include "../dbg/diag.h"
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

/* ---- Diagnostics state (UAOS-197/198/206/212) ---- */
volatile uint64_t g_ctx_switches = 0;   /* successful dispatches (watchdog) */
static uint64_t g_acct_last_tick = 0;   /* g_pit_ticks at last switch       */

/* Stack instrumentation: at task creation the whole stack is filled with
 * STACK_FILL so Task_StackPeakUsed() can report the high-water mark, and
 * the lowest qword carries STACK_CANARY — checked on every context switch
 * so a downward overflow into the base is caught even when it scribbles
 * into the neighbouring slot's stack first. */
#define STACK_FILL_BYTE 0xA5
#define STACK_CANARY    0xC0FFEE00C0FFEE01ULL

static void stack_instrument(uint8_t *stack)
{
    for (int i = 0; i < TASK_STACK_SIZE; i++) stack[i] = STACK_FILL_BYTE;
    *(volatile uint64_t *)stack = STACK_CANARY;
}

static void stack_canary_check(UaosTask *t)
{
    if (!t->native_stack_base || t->stack_overflowed) return;
    if (*(volatile uint64_t *)t->native_stack_base != STACK_CANARY) {
        t->stack_overflowed = 1;
        kprint("[TASK] STACK OVERFLOW: '");
        kprint(t->ln_Name ? t->ln_Name : "?");
        kprint("' base canary dead — stack grew past SPLower\n");
    }
}

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

/* Per-slot votes for the stranded-task scan in Task_WakeTimers — a task
 * must be seen unlinked-but-active on two consecutive ticks before it
 * is re-linked, so the list_init->enqueue window during task creation
 * is never mistaken for a strand (UAOS-265). */
static uint8_t g_strand_votes[MAX_TASKS];

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

/* Every queue primitive runs IRQ-atomic: the int $0x80 gate is a trap
 * gate that preserves IF, so do_schedule(0) reached via Task_Yield /
 * Task_CheckResched / Task_Exit executes these with interrupts live,
 * and task creation runs under Forbid which does not mask IRQs either.
 * An IRQ-side Signal/dequeue/schedule landing between the node and head
 * stores tears the list — observed as the EventPump node half-linked
 * into the pri-0 head (queue self-linked, node pointing in), stranding
 * a READY/WAITING task the scheduler could never reach (UAOS-265). */
void ready_enqueue(UaosTask *task)
{
    uint64_t fl = irq_save();
    int idx = pri_to_idx(task->ln_Pri);
    list_append(&g_ready_heads[idx], task);
    g_ready_map[idx >> 6] |= 1ULL << (idx & 63);
    task->tc_State = TASK_READY;
    irq_restore(fl);
}

/* Remove a task from its ready queue (e.g. SetTaskPri reprioritising).
 * Caller must guarantee the task is currently queued. */
void ready_remove(UaosTask *task)
{
    uint64_t fl = irq_save();
    int idx = pri_to_idx(task->ln_Pri);
    uint64_t bit = 1ULL << (idx & 63);
    list_remove(task);
    /* Clear-then-recheck: safe against an IRQ that enqueues to the same
     * queue between the list op and the bitmap op (ready_enqueue sets
     * the bit after appending, so either side restores it). */
    g_ready_map[idx >> 6] &= ~bit;
    if (!list_empty(&g_ready_heads[idx]))
        g_ready_map[idx >> 6] |= bit;
    irq_restore(fl);
}

static UaosTask *ready_dequeue_highest(void)
{
    UaosTask *res = NULL;
    uint64_t fl = irq_save();
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
            if (!t) continue;   /* map bit set but queue drained */
            if (t->tc_State != TASK_REMOVED) {
                res = t;
                goto out;
            }
            /* Marked REMOVED while still queued (e.g. RemTask): drop it. */
        }
    }
out:
    irq_restore(fl);
    return res;
}

/* -------------------------------------------------------------------------
 * Wait queue management
 * ------------------------------------------------------------------------- */

static void wait_enqueue(UaosTask *task)
{
    uint64_t fl = irq_save();
    list_append(&g_wait_head, task);
    g_wait_count++;
    task->tc_State = TASK_WAITING;
    irq_restore(fl);
}

void wait_remove(UaosTask *task)
{
    uint64_t fl = irq_save();
    list_remove(task);
    if (g_wait_count > 0) g_wait_count--;
    irq_restore(fl);
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
    /* REMOVED until ready_enqueue() births the task: g_task_count was
     * already bumped above, so IRQ-side walkers (WakeTimers, diag dumps)
     * can see this slot before it is fully built and linked. */
    t->tc_State = TASK_REMOVED;
    t->tc_IDNestCnt = 0;
    t->tc_TDNestCnt = 0;
    t->tc_SigAlloc = 0xFFFF;
    t->tc_SigWait = 0;
    t->tc_SigRecvd = 0;
    t->tc_SigExcept = 0;
    t->tc_SPLower = stack;
    t->tc_SPUpper = stack + TASK_STACK_SIZE;

    stack_instrument(stack);

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

    /* SysV ABI: at function entry RSP must be 8 mod 16, as if `call`
     * had just pushed a return address.  iretq jumps straight to entry,
     * so plant that slot ourselves — entering with a 16-aligned RSP
     * leaves the whole task misaligned and any compiler-emitted aligned
     * SSE spill (movaps/movdqa [rsp]) #GPs (UAOS-182: draw_menubar from
     * the Shell task; seen again via Jpeg_Encode, UAOS-218).
     * g_task_stacks is only aligned(8), so the top of the stack must be
     * forced to 16-alignment here rather than assumed from the array. */
    uint64_t *sp = (uint64_t *)((uintptr_t)(stack + TASK_STACK_SIZE) & ~15ull);
    *--sp = (uint64_t)Task_Exit;
    uint64_t entry_rsp = (uint64_t)sp;   /* ≡ 8 mod 16 — correct entry RSP */

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
    sp[20] = entry_rsp;                         /* RSP at entry (8 mod 16) */
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

    /* A REMOVED task can still be linked into a ready queue (RemTask on
     * a READY victim leaves it queued for the next dequeue to drop) —
     * unlink it before memset, or the zeroed node would sever whatever
     * list it sat in and strand its neighbours (UAOS-265).  Fresh slots
     * have ln_Succ == NULL and skip the unlink. */
    {
        uint64_t fl = irq_save();
        if (t->ln_Succ != NULL && t->ln_Succ != t)
            list_remove(t);
        irq_restore(fl);
    }

    for (int i = 0; i < (int)sizeof(UaosTask); i++)
        ((uint8_t *)t)[i] = 0;

    t->ln_Type = 1;  /* NT_TASK */
    t->ln_Pri = pri;
    t->ln_Name = name;
    list_init(t);

    t->tc_Flags = 0;
    /* REMOVED until ready_enqueue() births the task — see
     * Task_CreateNative. */
    t->tc_State = TASK_REMOVED;
    t->tc_IDNestCnt = 0;
    t->tc_TDNestCnt = 0;
    t->tc_SigAlloc = 0xFFFF;
    t->tc_SigWait = 0;
    t->tc_SigRecvd = 0;
    t->tc_SigExcept = 0;
    t->tc_SPLower = stack;
    t->tc_SPUpper = stack + TASK_STACK_SIZE;

    stack_instrument(stack);

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

    /* Queue surgery runs IRQ-atomic.  The syscall path (int $0x80 is a
     * trap gate — IF preserved) can be reached with interrupts live via
     * Task_Yield/Task_CheckResched/Task_Exit, so without this an IRQ
     * could tear the ready-queue mutations below mid-sequence — the
     * stranded-EventPump freeze (UAOS-265).  The queue primitives are
     * irq_save'd themselves as well, so callers that only touch one list
     * stay safe outside this window. */
    uint64_t sched_fl = irq_save();

    /* Charge the elapsed ticks to whoever currently owns the CPU — up
     * front, before the early-outs below.  Every path that returns
     * without dispatching leaves g_current running (armed switch pending,
     * suppressed reschedule, queues empty, or self-dispatch), so the
     * window belongs to it either way.  This also keeps the Idle task's
     * cpu_ticks advancing while it hlt-loops alone, which is what makes
     * CpuFreq_Tick()'s busy% and taskstat's idle share work (UAOS-272). */
    {
        uint64_t now = g_pit_ticks;
        g_current->cpu_ticks += now - g_acct_last_tick;
        g_acct_last_tick = now;
    }

    /* A syscall-side switch is armed but not yet consumed — defer (see
     * g_sched_switch_pending above).  Applies to both paths: an IRQ may
     * nest in the trap-gate window, and a nested int $0x80 issued before
     * the outer epilogue would see g_current already pointing at the
     * incoming task and misfile the physical RSP just the same. */
    if (g_sched_switch_pending) {
        g_need_resched = 1;
        irq_restore(sched_fl);
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
    if (Task_SwitchNext) {
        irq_restore(sched_fl);
        return;
    }

    if (from_irq) {
        /* Honour Forbid / Disable nesting — timer ISR only.  Record the
         * suppressed reschedule so Permit()/Enable() can dispatch it
         * when the nesting unwinds instead of dropping it outright. */
        if (g_current->tc_TDNestCnt > 0 || g_current->tc_IDNestCnt > 0) {
            g_need_resched = 1;
            irq_restore(sched_fl);
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
    if (!next) { irq_restore(sched_fl); return; }   /* nothing else to run */

    if (next == prev) {
        /* We were the only runnable task; keep running */
        g_current = prev;
        prev->tc_State = TASK_RUNNING;
        irq_restore(sched_fl);
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
    } else {
        /* Leaving an M68k context: rebind g_ram to the shared system
         * window.  Otherwise host-side writers (Intuition rendering,
         * chipset DMA, console echo) would keep targeting the suspended
         * guest's per-task RAM and corrupt it — observed as ASCII text
         * landing inside OctaMED's decrunch buffers. */
        extern uint8_t *g_shared_ram;
        g_ram = g_shared_ram;
    }

    /* ---- Accounting (taskstat / watchdog / irqaudit) ----
     * The tick charge already ran at entry; only the switch counters
     * remain here. */
    g_ctx_switches++;
    next->ctx_switches++;
    if (prev->tc_IDNestCnt > 0 || prev->tc_TDNestCnt > 0)
        prev->switch_while_crit++;
    stack_canary_check(prev);
    Etrace_Emit(ETRACE_SCHED, (uint32_t)(prev - g_tasks),
                (uint32_t)(next - g_tasks));

    /* Tell isr_common to perform the switch */
    Task_SwitchPrev = prev;
    Task_SwitchNext = next;

    irq_restore(sched_fl);
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
            /* Glue-side globals the guest may still hold: audio.device
             * channels/opens and a stale g_blocked_in flag (UAOS-247). */
            extern void UAOS_M68k_ReleaseTaskResources(UaosTask *t);
            UAOS_M68k_ReleaseTaskResources(g_current);
        }
        /* Reclaim files/locks the task left open — the handle table entry
         * (and the handler-side file/lock object behind it) would leak
         * otherwise (UAOS-247). */
        HandleTable_FreeByOwner(g_current);
        /* Retire the task's Intuition windows/screens before its RAM window
         * is released — armed slots would keep decoding dangling guest
         * pointers through whatever g_ram is bound (UAOS-265). */
        UAOS_Intuition_CleanupTask(g_current);
        /* Release M68k guest RAM so the slot can be reused. */
        Task_ReleaseM68kRam(g_current);
        /* Close any userspace sockets this task left open so the
         * usock/tcp slots are not leaked by a killed command. */
        usock_cleanup_task(g_current);
        /* Same reclaim for telnetd pump tasks (socket + session + pump
         * ctx — an ESTABLISHED socket has no stack-side bound) and for
         * remote shell tasks (remote_inuse slot) that die without
         * reaching their own exit paths (UAOS-263). */
        Telnetd_CleanupTask(g_current);
        ShellWin_RemoteCleanupTask(g_current);
        /* Drop the task's net RX-notify slot so a stale TCB pointer can
         * never be signalled by a later net_rx_kick(). */
        net_rx_notify_disarm(g_current);
        /* Critical-section leak audit (UAOS-206): a task that dies while
         * holding Disable/Forbid nesting leaves the counts pinned — warn
         * so irqaudit-style accounting catches the culprit. */
        if (g_current->tc_IDNestCnt > 0 || g_current->tc_TDNestCnt > 0) {
            kprint("[TASK] WARN: '");
            kprint(g_current->ln_Name ? g_current->ln_Name : "?");
            kprint("' exited with nesting ID=");
            kprinthex((uint64_t)(int)g_current->tc_IDNestCnt);
            kprint(" TD=");
            kprinthex((uint64_t)(int)g_current->tc_TDNestCnt);
            kprint("\n");
        }
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

int EventPump_IsCurrent(void)
{
    return g_eventpump_task != NULL && Task_Current() == g_eventpump_task;
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

        /* Raw key transitions -> focused window (IDCMP_RAWKEY path).
         * Drained before the cooked queue so RAWKEY precedes VANILLAKEY
         * for the same physical press, matching real AmigaOS ordering. */
        while (PS2Kbd_HasRawKey()) {
            int rk = PS2Kbd_GetRawKey();
            if (rk < 0) break;
            Blanker_OnInput();
            WM_RawKeyEvent(rk & 0xFF, (rk >> 8) & 0xFF);
        }

        /* Keyboard -> WM (Amiga key combos first, then command-key
         * shortcuts, then regular keys) */
        while (PS2Kbd_HasChar()) {
            char c = PS2Kbd_GetChar();
            unsigned char uc = (unsigned char)c;
            Blanker_OnInput();

            /* CIA-A SDR mirroring happens at IRQ time in ps2kbd via
             * chip_emu_push_keycode for every real key transition — no
             * cooked-char feed here (that path raced a second kbuf
             * consumer in the PIT poll). */

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

            /* Amiga+letter — menu shortcut via Intuition command key.
             * On a miss the real keymap suppresses the vanilla char, so
             * nothing further is delivered (the RAWKEY pair already went). */
            if (IS_AMIGA_RKEY(c)) {
                char letter = AMIGA_RLETTER(c);
                Intuition_InvokeCommandKey(letter);
                continue;
            }

            /* Regular key — try command key first, then WM */
            if (!Intuition_InvokeCommandKey(c))
                WM_KeyEvent(c);
        }

        /* Clock redraw */
        Desktop_FlushClockRedraw();

        /* Screen-blank request from the RTC-tick path (UAOS-191): the
         * IRQ only flags it — the black-frame paint runs here in task
         * context under Forbid like every other framebuffer frame. */
        Blanker_Flush();

        /* Network */
        net_stack_poll();

        /* Guest-owned front screen: apps like OctaMED draw straight into
         * their screen BitMap's planes with CPU stores — no library call
         * to hook.  Poll-marks the screen damaged so FlushRedraw re-decodes
         * the planes (~hardware bitmap fetch), and shortens the wait below
         * so the refresh runs at ~20 Hz instead of the 100-tick fallback. */
        int guest_screen_front = UAOS_Intuition_PollFrontScreenBitmap();

        /* Coalesced repaint: event handlers accumulate damage instead of
         * repainting per event — flush once per iteration (UAOS-101).
         * Then apply any IRQ-deferred cursor move (UAOS-104). */
        WM_FlushRedraw();
        Cursor_Flush();

        Permit();
        /* --- End protected section --- */

        /* Background jobs — dispatched OUTSIDE the Forbid region: the
         * job pump runs arbitrary command bodies that legitimately
         * block (yield_ms sleeps, remote-shell TX waits, filesystem
         * packet round-trips), and blocking while Forbid'd deschedules
         * the pump mid-critical-section — the UAOS-169/170/176 bug
         * class counted by irqaudit's crit-sw (UAOS-271).  bg_run_next
         * takes its own Forbid for queue surgery only. */
        if (!PS2Kbd_HasChar())
            ShellWin_PollJobs();

        /* UAOS-265: cheap back-buffer/VRAM consistency spot-check. */
        FB_Watchdog();

        /* Block until the next event instead of spinning on hlt: the
         * producers above Signal() us and IRQ-side wakes reschedule at
         * interrupt exit (or at Permit via g_need_resched).  Signals
         * that landed during the work body are latched in tc_SigRecvd,
         * so this returns immediately when there is pending input.
         * The timeout is only a safety net for unsignalled producers. */
        Task_WaitTicks(SIGF_EVENTPUMP | SIGF_NET | SIGF_CHILD,
                       guest_screen_front ? 5 : 100);
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
    Etrace_Emit(ETRACE_SIGNAL, (uint32_t)(task - g_tasks), sigmask);
    irq_restore(fl);
}

/* UAOS-271 diagnostic: entering a blocking primitive while Disable()/
 * Forbid() nested deschedules the task mid-critical-section — exactly
 * the UAOS-169/170/176 bug class irqaudit's crit-sw column counts.
 * Log the caller PC so the offending path can be symbolized instead of
 * inferred from the bare counter. */
static void crit_block_warn(const char *fn, void *caller)
{
    if (!g_current ||
        (g_current->tc_IDNestCnt <= 0 && g_current->tc_TDNestCnt <= 0))
        return;
    kprint("[TASK] WARN: '");
    kprint(g_current->ln_Name ? g_current->ln_Name : "?");
    kprint("' blocked in ");
    kprint(fn);
    kprint("() while critical (ID=");
    kprinthex((uint64_t)(int)g_current->tc_IDNestCnt);
    kprint(" TD=");
    kprinthex((uint64_t)(int)g_current->tc_TDNestCnt);
    kprint(") caller=");
    kprinthex((uint64_t)(uintptr_t)caller);
    kprint("\n");
}

uint32_t Wait(uint32_t sigmask)
{
    uint32_t result;

    crit_block_warn("Wait", __builtin_return_address(0));

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
    crit_block_warn("Task_SleepTicks", __builtin_return_address(0));
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
    crit_block_warn("Task_WaitTicks", __builtin_return_address(0));
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

    /* Stranded-task safety net (UAOS-265): if a queue op is ever torn —
     * a task left READY/WAITING with self-linked node pointers is not in
     * any list, so neither a Signal() wake nor a timer expiry can ever
     * reach it (the EventPump froze exactly like this, killing input,
     * WM redraw and cursor while the rest of the kernel stayed live).
     * Two consecutive ticks unlinked = stranded; re-link and log so the
     * residual cause stays visible instead of silently freezing.  The
     * 2-tick vote skips the brief list_init->enqueue window of a task
     * under construction (which is now marked REMOVED until linked). */
    for (int i = 0; i < g_task_count; i++) {
        UaosTask *s = &g_tasks[i];
        uint8_t st = s->tc_State;
        if (s == g_current || (st != TASK_READY && st != TASK_WAITING) ||
            s->ln_Succ != s || s->ln_Pred != s) {
            g_strand_votes[i] = 0;
            continue;
        }
        if (++g_strand_votes[i] < 2)
            continue;
        g_strand_votes[i] = 0;
        kprint("[TASK] WARN: '");
        kprint(s->ln_Name ? s->ln_Name : "?");
        kprint(st == TASK_READY ? "' stranded READY-unlinked — re-queued\n"
                                : "' stranded WAITING-unlinked — re-queued\n");
        if (st == TASK_READY) ready_enqueue(s);
        else                wait_enqueue(s);
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
    if (g_current) {
        /* IRQOFF hold-time audit (UAOS-206): stamp the TSC on the 0->1
         * transition; Enable() measures the hold in cycles because
         * g_pit_ticks is frozen while IF=0. */
        if (g_current->tc_IDNestCnt == 0)
            g_current->disable_enter_tick = diag_rdtsc();
        g_current->tc_IDNestCnt++;
    }
}

void Enable(void)
{
    if (!g_current) {
        __asm__ volatile ("sti");
        return;
    }
    if (--g_current->tc_IDNestCnt <= 0) {
        g_current->tc_IDNestCnt = 0;
        /* Close the IF=0 hold interval started by Disable().  PIT ticks
         * cannot advance while IF=0 so this is measured in TSC cycles,
         * converted to ms with the self-calibrated TSC frequency —
         * a hold that long is a bug signature, not a critical section. */
        uint64_t dur = diag_rdtsc() - g_current->disable_enter_tick;
        g_current->irqoff_ticks += dur;
        if (dur > g_current->irqoff_max_ticks)
            g_current->irqoff_max_ticks = dur;
        uint64_t hz = Tickmon_TscHz();
        /* Uncalibrated (first ~100 PIT ticks): ~50M cycles is roughly
         * 17-100 ms depending on the machine — close enough. */
        uint64_t thresh = hz ? hz / (1000 / TASK_IRQOFF_LONG_MS)
                             : 50000000ULL;
        if (dur > thresh) {
            g_current->irqoff_long++;
            kprint("[TASK] WARN: '");
            kprint(g_current->ln_Name ? g_current->ln_Name : "?");
            kprint("' held Disable() for ");
            if (hz) {
                kprintdec((uint32_t)(dur * 1000 / hz));
                kprint(" ms (");
                kprinthex(dur);
                kprint(" cycles)");
            } else {
                kprinthex(dur);
                kprint(" cycles");
            }
            kprint(" (IF=0)\n");
        }
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

/* =========================================================================
 * Diagnostics — taskdump / taskstat / watchdog helpers (UAOS-195/197/198)
 * ========================================================================= */

int Task_RunnableCount(void)
{
    int n = 0;
    for (int i = 0; i < g_task_count; i++) {
        uint8_t st = g_tasks[i].tc_State;
        if (st == TASK_RUNNING || st == TASK_READY) n++;
    }
    return n;
}

uint32_t Task_StackPeakUsed(UaosTask *t)
{
    if (!t || !t->native_stack_base) return 0;
    const uint8_t *base = (const uint8_t *)t->native_stack_base;
    uint32_t size = t->native_stack_size;
    /* First 8 bytes are the canary; scan upward for the lowest byte that
     * is no longer fill pattern — watermark of deepest stack usage. */
    uint32_t i = 8;
    while (i < size && base[i] == STACK_FILL_BYTE) i++;
    return size - i;   /* bytes ever touched (upper bound estimate) */
}

static const char *task_state_name(uint8_t st)
{
    switch (st) {
    case TASK_RUNNING: return "running";
    case TASK_READY:   return "ready";
    case TASK_WAITING: return "waiting";
    case TASK_REMOVED: return "removed";
    default:           return "?";
    }
}

static const char *task_type_name(TaskType ty)
{
    switch (ty) {
    case TASK_TYPE_NATIVE: return "native";
    case TASK_TYPE_M68K:   return "m68k";
    case TASK_TYPE_X64:    return "x64";
    default:               return "?";
    }
}

/* Decode the saved interrupt frame parked at native_rsp — same layout the
 * isr_common prologue/creation stubs build (see Task_CreateNative). */
static void task_dump_frame(UaosTask *t, void *ctx, DiagEmitFn emit)
{
    DiagLine l;
    const uint64_t *sp = (const uint64_t *)(uintptr_t)t->native_rsp;

    static const char *const rnames[15] = {
        "r15","r14","r13","r12","r11","r10","r9","r8",
        "rbp","rdi","rsi","rdx","rcx","rbx","rax"
    };

    dl_reset(&l);
    dl_add(&l, "  saved frame @"); dl_hex(&l, t->native_rsp);
    dl_add(&l, "  (armed=");
    dl_add(&l, (t == Task_SwitchNext) ? "yes" : "no");
    dl_add(&l, ")");
    dl_emit(&l, ctx, emit);

    /* Only safe to decode when the frame is within this task's stack. */
    uint64_t lo = (uint64_t)(uintptr_t)t->native_stack_base;
    uint64_t hi = lo + t->native_stack_size;
    if (t->native_rsp < lo || t->native_rsp + 22 * 8 > hi) {
        dl_add(&l, "  <native_rsp outside stack range — frame stale/corrupt>");
        dl_emit(&l, ctx, emit);
        return;
    }

    dl_add(&l, "  rip=");
    dl_hex(&l, sp[17]);
    dl_add(&l, "  cs=");  dl_hex(&l, sp[18]);
    dl_add(&l, "  rflags="); dl_hex(&l, sp[19]);
    dl_add(&l, "  rsp="); dl_hex(&l, sp[20]);
    dl_add(&l, "  ss=");  dl_hex(&l, sp[21]);
    dl_emit(&l, ctx, emit);

    dl_add(&l, "  vec="); dl_hex(&l, sp[15]);
    dl_add(&l, "  err="); dl_hex(&l, sp[16]);
    dl_emit(&l, ctx, emit);

    for (int i = 0; i < 15; i += 5) {
        dl_reset(&l);
        for (int j = i; j < i + 5; j++) {
            dl_ch(&l, ' ');
            dl_add(&l, rnames[j]);
            dl_ch(&l, '=');
            dl_hex(&l, sp[j]);
        }
        dl_emit(&l, ctx, emit);
    }
}

void Task_DiagDump(void *ctx, void (*emit)(void *ctx, const char *line),
                   const char *name, int full)
{
    DiagLine l;

    for (int i = 0; i < g_task_count; i++) {
        UaosTask *t = &g_tasks[i];
        if (t->tc_State == TASK_REMOVED) continue;
        if (name && (!t->ln_Name || !name[0])) continue;
        if (name) {
            /* case-insensitive substring-free exact match */
            const char *a = t->ln_Name, *b = name;
            int eq = 1;
            while (*a && *b) {
                char ca = *a, cb = *b;
                if (ca >= 'A' && ca <= 'Z') ca += 32;
                if (cb >= 'A' && cb <= 'Z') cb += 32;
                if (ca != cb) { eq = 0; break; }
                a++; b++;
            }
            if (!eq || *a || *b) continue;
        }

        dl_reset(&l);
        dl_add(&l, " #"); dl_dec(&l, (uint64_t)i); dl_pad(&l, 4);
        dl_ch(&l, ' ');
        dl_add(&l, t->ln_Name ? t->ln_Name : "(null)"); dl_pad(&l, 24);
        dl_add(&l, task_type_name(t->type)); dl_pad(&l, 31);
        dl_add(&l, task_state_name(t->tc_State)); dl_pad(&l, 39);
        dl_add(&l, "pri="); dl_sdec(&l, t->ln_Pri);
        dl_add(&l, " cpu="); dl_dec(&l, t->cpu_ticks);
        dl_add(&l, "t sw="); dl_dec(&l, t->ctx_switches);
        dl_add(&l, " pk="); dl_dec(&l, Task_StackPeakUsed(t));
        dl_add(&l, "/"); dl_dec(&l, t->native_stack_size);
        if (t->tc_SigWait) { dl_add(&l, " wait="); dl_hex(&l, t->tc_SigWait); }
        if (t->tc_IDNestCnt || t->tc_TDNestCnt) {
            dl_add(&l, " nest D="); dl_sdec(&l, t->tc_IDNestCnt);
            dl_add(&l, " F="); dl_sdec(&l, t->tc_TDNestCnt);
        }
        if (t == g_current) dl_add(&l, " <-cur");
        dl_emit(&l, ctx, emit);

        if (full) {
            dl_add(&l, "  stack=["); dl_hex(&l, (uint64_t)(uintptr_t)t->native_stack_base);
            dl_add(&l, ".."); dl_hex(&l, (uint64_t)(uintptr_t)t->native_stack_base + t->native_stack_size);
            dl_add(&l, "]");
            if (t->stack_overflowed) dl_add(&l, "  ** CANARY DEAD **");
            dl_emit(&l, ctx, emit);

            if (t->type == TASK_TYPE_M68K) {
                dl_add(&l, "  m68k: pc-entry="); dl_hex(&l, t->m68k_entry);
                dl_add(&l, " sp="); dl_hex(&l, t->m68k_stack_top);
                dl_add(&l, " ctxsz="); dl_dec(&l, t->m68k_context_size);
                dl_add(&l, " cycles(rem/init)=");
                dl_dec(&l, (uint64_t)(uint32_t)t->m68k_remaining_cycles);
                dl_ch(&l, '/');
                dl_dec(&l, (uint64_t)(uint32_t)t->m68k_initial_cycles);
                dl_emit(&l, ctx, emit);
                /* Saved Musashi context: dar[16]+dar_save[16] then ppc,pc.
                 * dar[0..7]=d0-d7, dar[8..15]=a0-a7. */
                if (t->m68k_context_buf) {
                    const uint32_t *cr =
                        (const uint32_t *)t->m68k_context_buf;
                    dl_add(&l, "  m68kctx: pc="); dl_hex(&l, cr[33]);
                    dl_add(&l, " ppc="); dl_hex(&l, cr[32]);
                    dl_add(&l, " d0="); dl_hex(&l, cr[0]);
                    dl_add(&l, " a3="); dl_hex(&l, cr[8 + 3]);
                    dl_add(&l, " a4="); dl_hex(&l, cr[8 + 4]);
                    dl_add(&l, " a6="); dl_hex(&l, cr[8 + 6]);
                    dl_emit(&l, ctx, emit);
                }
            } else {
                task_dump_frame(t, ctx, emit);
            }
        }
    }
}
