/* task.c — UAOS Unified Task Scheduler
 *
 * Priority-based round-robin for native x86_64 tasks.
 * Phase 1: native tasks only; M68k task fields are reserved.
 */

#include "task.h"
#include "../boot/kprint.h"
#include "../irq/ps2mouse.h"
#include "../irq/ps2kbd.h"
#include "../display/framebuffer.h"
#include "../display/desktop.h"
#include "../display/wm.h"
#include "../net/stack.h"
#include "../display/shell_win.h"
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
uint8_t  g_task_stacks[MAX_TASKS][TASK_STACK_SIZE];
int      g_task_count = 0;
static UaosTask *g_current = NULL;
UaosTask *Task_SwitchNext = NULL;
UaosTask *Task_SwitchPrev = NULL;

/* Ready queues: one doubly-linked list per priority level */
static UaosTask g_ready_heads[256];  /* index 0 = pri -128 */
static int      g_ready_mask = 0;    /* bit i set if ready queue at pri i-128 is non-empty */

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
    g_ready_mask |= (1 << (idx & 31));   /* simplified: we only track first 32 priorities for now */
    task->tc_State = TASK_READY;
}

static UaosTask *ready_dequeue_highest(void)
{
    /* Scan from highest priority (255) down to 0 */
    for (int idx = 255; idx >= 0; idx--) {
        if (!list_empty(&g_ready_heads[idx])) {
            UaosTask *t;
            list_remove_head(&g_ready_heads[idx], &t);
            return t;
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
 * Task creation
 * ------------------------------------------------------------------------- */

UaosTask *Task_CreateNative(const char *name, int8_t pri,
                            void (*entry)(void *), void *arg)
{
    if (g_task_count >= MAX_TASKS) return NULL;

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
    return t;
}

/* -------------------------------------------------------------------------
 * Scheduling
 * ------------------------------------------------------------------------- */

static void do_schedule(int from_irq)
{
    if (!g_current) return;

    if (from_irq) {
        /* Honour Forbid / Disable nesting — timer ISR only */
        if (g_current->tc_TDNestCnt > 0 || g_current->tc_IDNestCnt > 0)
            return;
    }

    UaosTask *next = ready_dequeue_highest();
    if (!next) return;   /* nothing else to run */

    if (next == g_current) {
        if (from_irq) ready_enqueue(next);
        return;
    }

    UaosTask *prev = g_current;
    if (prev->tc_State == TASK_RUNNING) {
        prev->tc_State = TASK_READY;
        ready_enqueue(prev);
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

void Task_Yield(void)
{
    /* Under the timer-driven scheduler, voluntary yield is a no-op.
     * The timer ISR will preempt us at the next tick boundary.
     * Calling Task_ScheduleFromIRQ from normal task context corrupts
     * g_current because the current task context has not been saved. */
    __asm__ volatile ("pause");
}

void Task_Exit(void)
{
    /* Mark task as removed so the scheduler never picks it again.
     * Then halt with interrupts enabled so the timer ISR can
     * switch to another task.  The task becomes a zombie. */
    if (g_current) g_current->tc_State = TASK_REMOVED;
    for (;;) __asm__ volatile ("sti; hlt");
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
    g_ready_mask = 0;
    list_init(&g_wait_head);
    g_wait_count = 0;

    kprint("[TASK] Scheduler initialised\n");
}

void Task_StartFirst(void)
{
    kprint("[TASK] Task_StartFirst called, count="); kprinthex(g_task_count); kprint("\n");
    UaosTask *first = ready_dequeue_highest();
    if (first) {
        kprint("[TASK] Starting first task: "); kprint(first->ln_Name); kprint("\n");
        kprint("[TASK] entry="); kprinthex((uint64_t)(uintptr_t)first->native_entry);
        kprint(" stack="); kprinthex((uint64_t)(uintptr_t)first->native_stack_base);
        kprint(" size="); kprinthex(first->native_stack_size);
        kprint(" rsp="); kprinthex(first->native_rsp); kprint("\n");
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
 * Idle task — former main event loop
 * ------------------------------------------------------------------------- */
void Task_IdleEntry(void *arg)
{
    (void)arg;
    int last_mx = -1, last_my = -1, last_btn = -1;
    uint64_t loop_count = 0;

    kprint("[IDLE] System idle task started\n");

    for (;;) {
        __asm__ volatile ("pause" ::: "memory");

        /* Mouse -> WM */
        if (g_fb.valid) {
            int mx = g_mouse.x, my = g_mouse.y, btn = g_mouse.btn_left;
            if (mx != last_mx || my != last_my || btn != last_btn) {
                last_mx = mx; last_my = my; last_btn = btn;
                WM_MouseEvent(mx, my, btn);
            }
        }

        /* Keyboard -> WM */
        while (PS2Kbd_HasChar())
            WM_KeyEvent(PS2Kbd_GetChar());

        /* Clock redraw */
        Desktop_FlushClockRedraw();

        /* Network */
        net_stack_poll();

        /* Background jobs */
        if (!PS2Kbd_HasChar())
            ShellWin_PollJobs();

        /* Heartbeat */
        loop_count++;
        if ((loop_count & 0x7FFFFFF) == 0) {
            kprint("[IDLE] loop="); kprinthex(loop_count);
            kprint(" pit="); kprinthex(g_pit_ticks);
            kprint("\n");
        }
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

    __asm__ volatile ("cli");
    task->tc_SigRecvd |= sigmask;

    if (task->tc_State == TASK_WAITING && (task->tc_SigRecvd & task->tc_SigWait) != 0) {
        wait_remove(task);
        ready_enqueue(task);
    }
    __asm__ volatile ("sti");
}

uint32_t Wait(uint32_t sigmask)
{
    uint32_t result;

    __asm__ volatile ("cli");
    g_current->tc_SigWait = sigmask;

    while ((g_current->tc_SigRecvd & sigmask) == 0) {
        g_current->tc_State = TASK_WAITING;
        wait_enqueue(g_current);
        __asm__ volatile ("int $0x80");
    }

    result = g_current->tc_SigRecvd & sigmask;
    g_current->tc_SigRecvd &= ~sigmask;
    g_current->tc_SigWait = 0;
    __asm__ volatile ("sti");

    return result;
}

uint32_t SetSignal(uint32_t newsignals, uint32_t sigmask)
{
    __asm__ volatile ("cli");
    uint32_t old = g_current->tc_SigRecvd;
    g_current->tc_SigRecvd = (old & ~sigmask) | (newsignals & sigmask);
    __asm__ volatile ("sti");
    return old;
}

void Forbid(void)
{
    if (g_current) g_current->tc_TDNestCnt++;
}

void Permit(void)
{
    if (!g_current) return;
    if (--g_current->tc_TDNestCnt <= 0) {
        g_current->tc_TDNestCnt = 0;
        /* A task switch is deferred to the next timer tick */
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
    }
}
