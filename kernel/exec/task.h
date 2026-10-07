/* task.h — UAOS Unified Task Scheduler
 *
 * Supports both native x86_64 tasks and M68k guest tasks with AmigaOS 3.1
 * compatible fields.
 */

#ifndef UAOS_TASK_H
#define UAOS_TASK_H

#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * AmigaOS 3.1 compatible task states
 * ------------------------------------------------------------------------- */
#define TASK_RUNNING   2
#define TASK_READY     1
#define TASK_WAITING   0
#define TASK_REMOVED   3

/* -------------------------------------------------------------------------
 * AmigaOS 3.1 signal bits
 * ------------------------------------------------------------------------- */
#define SIGB_ABORT     0
#define SIGB_CHILD     1
#define SIGB_BLIT      4
#define SIGB_SINGLE    4
#define SIGB_INTUITION 5
#define SIGB_NET       7
#define SIGB_TP        6   /* bcm5974 mode-reset worker */
#define SIGB_BREAKF    8
#define SIGB_EVENTPUMP 9   /* wake bit for the EventPump service task */
#define SIGB_ATP       10  /* appletouch (Geyser) idle-reinit worker */

#define SIGF_ABORT     (1U << SIGB_ABORT)
#define SIGF_CHILD     (1U << SIGB_CHILD)
#define SIGF_BLIT      (1U << SIGB_BLIT)
#define SIGF_SINGLE    (1U << SIGB_SINGLE)
#define SIGF_INTUITION (1U << SIGB_INTUITION)
#define SIGF_NET       (1U << SIGB_NET)
#define SIGF_TP        (1U << SIGB_TP)
#define SIGF_BREAKF    (1U << SIGB_BREAKF)
#define SIGF_EVENTPUMP (1U << SIGB_EVENTPUMP)
#define SIGF_ATP       (1U << SIGB_ATP)

/* -------------------------------------------------------------------------
 * Task type
 * ------------------------------------------------------------------------- */
typedef enum {
    TASK_TYPE_NATIVE = 0,
    TASK_TYPE_M68K   = 1,
    TASK_TYPE_X64    = 2,
} TaskType;

/* -------------------------------------------------------------------------
 * Unified Task Control Block
 *
 * AmigaOS 3.1 Task struct fields are embedded for exact compatibility.
 * Native-specific fields follow.
 * ------------------------------------------------------------------------- */
typedef struct UaosTask {
    /* Node header (AmigaOS compatible) */
    struct UaosTask *ln_Succ;
    struct UaosTask *ln_Pred;
    uint8_t          ln_Type;
    int8_t           ln_Pri;
    const char      *ln_Name;

    /* AmigaOS Task fields */
    uint8_t  tc_Flags;
    uint8_t  tc_State;
    int8_t   tc_IDNestCnt;   /* Disable nesting */
    int8_t   tc_TDNestCnt;   /* Forbid nesting */
    uint32_t tc_SigAlloc;
    uint32_t tc_SigWait;
    uint32_t tc_SigRecvd;
    uint32_t tc_SigExcept;
    uint16_t tc_TrapAlloc;
    uint16_t tc_TrapAble;
    void    *tc_ExceptData;
    void    *tc_ExceptCode;
    void    *tc_TrapData;
    void    *tc_TrapCode;
    void    *tc_SPReg;       /* stack pointer (for M68k tasks: guest SP) */
    void    *tc_SPUpper;
    void    *tc_SPLower;
    void    *tc_MemEntry;
    void    *tc_UserData;

    /* Native x86_64 fields */
    TaskType type;
    uint64_t native_rsp;     /* saved x86_64 RSP */
    uint64_t native_rip;     /* saved x86_64 RIP (entry point) */
    void    *native_stack_base;
    uint32_t native_stack_size;
    void   (*native_entry)(void *arg);
    void    *native_arg;
    uint64_t native_initial_rsp;  /* ELF64 user stack pointer at entry */

    /* M68k guest fields */
    uint32_t m68k_task_struct;    /* guest RAM address of AmigaOS Task struct */
    uint32_t m68k_context_size;
    void    *m68k_context_buf;    /* host buffer for m68k_get_context/set_context */
    uint8_t *m68k_ram;            /* per-task guest RAM (NULL for native tasks) */
    uint32_t m68k_entry;          /* guest PC entry point */
    uint32_t m68k_stack_top;      /* guest SP */
    uint32_t m68k_bin_size;       /* size of loaded binary */
    uint32_t m68k_bin_save_off;   /* offset of saved binary copy in guest RAM */
    int      m68k_initial_cycles; /* saved m68ki_initial_cycles */
    int      m68k_remaining_cycles; /* saved m68ki_remaining_cycles */
    uint8_t  m68k_halted;       /* set when dos_Exit called */
    uint8_t  m68k_budget_dumped;/* one-shot PC-ring dump at soft cycle budget */
    void    *m68k_print_fn;       /* GluePrintFn for output */

    /* M68k CLI argument storage — copied at creation so asynchronous
     * tasks keep their argument vector alive after the caller returns. */
    char       m68k_name[16];       /* persistent copy of task name */
    const char *m68k_argv[18];      /* argv[0] = name, argv[1..] = args */
    char       m68k_argv_store[256]; /* string storage for m68k_argv */

    /* Scheduling */
    uint32_t time_slice_ticks;
    uint32_t ticks_remaining;

    /* Per-M68k-task signal bit used by graphics.library/WaitTOF to block until
     * the next VBlank.  -1 means no signal bit was allocated. */
    int8_t   m68k_vblank_sig;

    /* Parent task (for SIGF_CHILD notification on exit). */
    struct UaosTask *parent;

    /* Shell background-job number that spawned this task (0 = none).
     * Stamped at creation from g_task_bg_job so the job pump can tell
     * whether a backgrounded command is still running even after its
     * task slot has been recycled. */
    int32_t  bg_job;

    /* Absolute g_pit_ticks deadline at which a task parked by
     * Task_SleepTicks wakes.  0 = not sleeping.  Scanned every PIT
     * tick by Task_WakeTimers. */
    uint64_t tc_wake_tick;

    /* Per-task current working directory (copied at creation). */
    char     task_cwd[128];

    /* Per-task guest BPTR from dos SetProgramDir/GetProgramDir — the
     * lock address lives in this task's guest RAM window, so sharing it
     * across M68k tasks would hand one task a pointer into another's
     * arena. */
    uint32_t m68k_program_dir;

    /* Output routing for X64/native userspace tasks.
     * raw output is accumulated in task_out and flushed line-by-line
     * through native_print_fn when a newline is seen or on task exit. */
    void   (*native_print_fn)(void *ctx, const char *line);
    void    *native_print_ctx;
    char     task_out[256];
    int      task_out_len;

    /* ---- Diagnostics (taskstat / irqaudit / stack watermark) ---- */
    uint64_t cpu_ticks;         /* PIT ticks charged while running     */
    uint32_t ctx_switches;      /* times this task was dispatched      */
    uint64_t irqoff_ticks;      /* total ticks spent in Disable()      */
    uint64_t irqoff_max_ticks;  /* longest single Disable() hold       */
    uint32_t irqoff_long;       /* holds exceeding IRQAUDIT threshold  */
    uint32_t switch_while_crit; /* descheduled while Disable/Forbid held */
    uint64_t disable_enter_tick;/* TSC when IDNest went 0->1           */
    uint8_t  stack_overflowed;  /* stack-base canary was scribbled on  */
} UaosTask;

/* -------------------------------------------------------------------------
 * Scheduler API
 * ------------------------------------------------------------------------- */

#define MAX_TASKS      64
#define TASK_STACK_SIZE  32768   /* 32 KB per native task */
#define MAX_PRI         127
#define MIN_PRI        -128

/* Initialise scheduler (called once at boot) */
void TaskScheduler_Init(void);

/* Create a native x86_64 task. Returns task pointer or NULL. */
UaosTask *Task_CreateNative(const char *name, int8_t pri,
                            void (*entry)(void *), void *arg);

/* Create an x86-64 native task from an ELF64-loaded image.
 * entry_rip:    initial instruction pointer
 * initial_rsp:  initial user stack pointer (e.g. ELF64 loader result)
 * cwd:          current working directory string (copied into task)
 * print_fn:     line printer that receives flushed stdout lines
 * print_ctx:    opaque context passed to print_fn
 * Returns task pointer or NULL. */
UaosTask *Task_CreateX64(const char *name, int8_t pri,
                         uint64_t entry_rip, uint64_t initial_rsp,
                         const char *cwd,
                         void (*print_fn)(void *ctx, const char *line),
                         void *print_ctx);

/* Create an M68k guest task (native wrapper that runs a binary).
 * Returns task pointer or NULL. */
UaosTask *Task_CreateM68k(const char *name, int8_t pri,
                           const uint8_t *binary, uint32_t bin_size,
                           const char **argv,
                           void (*print_fn)(const char *));

/* Release an M68k task's guest RAM slot (called by Task_Exit) */
void Task_ReleaseM68kRam(UaosTask *t);

/* Current task yields CPU voluntarily.  Performs a real reschedule via
 * the syscall ISR (UAOS-169) and returns nonzero when the CPU was handed
 * to another task.  Returns 0 — without switching — when nothing else is
 * runnable, when called from IRQ context, or while Forbid/Disable
 * nesting is held. */
int  Task_Yield(void);

/* Exit current task */
void Task_Exit(void) __attribute__((noreturn));

/* Called from timer ISR when preemption is allowed */
void Task_ScheduleFromIRQ(void);

/* Get currently running task */
UaosTask *Task_Current(void);

/* Change a task's scheduler priority (clamped to [MIN_PRI, MAX_PRI]) */
void SetTaskPri(UaosTask *task, int newpri);

/* Idle task entry (system loop) */
void Task_IdleEntry(void *arg);
void Task_EventPumpEntry(void *arg);

/* Assembly: switch from current task to next task */
void Task_SwitchContext(UaosTask *old_task, UaosTask *new_task);

/* Assembly: return to a new task's initial entry point */
void Task_RunNew(UaosTask *task);

/* Start the first ready task (called once at boot with interrupts off) */
void Task_StartFirst(void);

/* Global used by assembly ISR to perform context switch */
extern UaosTask *Task_SwitchNext;
extern UaosTask *Task_SwitchPrev;

/* Task currently blocked inside graphics.library/WaitTOF, signalled by the
 * VBlank path in timer_ProcessTicks().  Defined in graphics_lib.c. */
extern UaosTask *g_wait_tof_task;

/* IRQ nesting depth (defined in irq/idt.c): >0 while a hardware interrupt
 * frame is being serviced.  Used to keep task-context operations (deferred
 * reschedules, EventPump wake dispatch) out of ISR bodies. */
extern volatile int g_irq_depth;

/* Signal the EventPump task (SIGF_EVENTPUMP) — IRQ- and task-context safe.
 * Producers: PS/2 kbd/mouse, USB-HID, bcm5974 trackpad, RTC second tick,
 * WM damage, background-job enqueue.  When called from IRQ context it also
 * requests a reschedule so the pump can run at interrupt exit. */
void EventPump_Wake(void);

/* Non-zero when the calling context IS the event pump task.  Used by the
 * WM to decide whether a repaint request may paint synchronously (pump
 * context, already serialized) or must be deferred as pending damage
 * (any other preemptible task context). */
int  EventPump_IsCurrent(void);

/* -------------------------------------------------------------------------
 * Signal / Wait / Critical sections
 * ------------------------------------------------------------------------- */

uint32_t Wait(uint32_t sigmask);

/* Block the current task for `ticks` PIT ticks (100 Hz = 10 ms each).
 * Implemented as a timed wait: the task sits on the wait queue with a
 * tc_wake_tick deadline and is re-readied by Task_WakeTimers.  Unlike
 * Wait() it does not take signals — it is purely time-driven. */
void     Task_SleepTicks(uint64_t ticks);

/* Wait() with a timeout: block until any signal in `sigmask` arrives or
 * `ticks` PIT ticks (100 Hz) elapse, whichever comes first.  Returns the
 * received bits (0 on timeout).  Lets event-driven consumers (e.g. net
 * RX via SIGF_NET) wake at interrupt time instead of the next tick. */
uint32_t Task_WaitTicks(uint32_t sigmask, uint64_t ticks);

/* Re-ready wait-queue tasks whose tc_wake_tick deadline has passed.
 * Called from the PIT interrupt handler once per tick. */
void     Task_WakeTimers(void);
void Task_ClearSig(uint32_t sigmask);
void     Signal(UaosTask *task, uint32_t sigmask);
uint32_t SetSignal(uint32_t newsignals, uint32_t sigmask);
void     Forbid(void);
void     Permit(void);
void     Disable(void);
void     Enable(void);

/* Syscall entry — called from vector 0x80 ISR for voluntary task switch */
void Task_ScheduleFromSyscall(void);

/* -------------------------------------------------------------------------
 * Internal scheduler helpers (used by exec_signal.c / exec_ipc.c)
 * ------------------------------------------------------------------------- */

void ready_enqueue(UaosTask *task);
void ready_remove(UaosTask *task);
void wait_remove(UaosTask *task);

extern UaosTask g_tasks[];
extern int      g_task_count;

/* Set by the shell job pump while dispatching a background job; every
 * task created during that dispatch is stamped with the job number. */
extern int32_t  g_task_bg_job;

/* M68k task lookup */
UaosTask *Task_FindByM68kAddr(uint32_t guest_addr);
UaosTask *Task_FindByName(const char *name);

/* Query scheduler state for status bar */
void Task_GetCounts(int *out_total, int *out_running, int *out_waiting);

/* Report emulated M68k guest RAM slot usage (per-task RAM pools). */
void Task_M68kSlotCount(int *total, int *used);

/* Test helper: spawn UART-printing tasks */
void Task_TestSpawn(void);

/* -------------------------------------------------------------------------
 * Diagnostics (UAOS-195/197/198/206/212)
 * ------------------------------------------------------------------------- */

/* Total context switches since boot — the watchdog's progress signal. */
extern volatile uint64_t g_ctx_switches;

/* Number of tasks in TASK_RUNNING or TASK_READY right now.  The watchdog
 * uses this to suppress false positives when only one task can run. */
int      Task_RunnableCount(void);

/* Scan the stack fill pattern: returns the highest byte count ever used
 * (watermark = SPUpper minus the lowest non-0xA5 byte).  Includes a base
 * canary check — 0xFFFFFFFF on a bad stack pointer range. */
uint32_t Task_StackPeakUsed(UaosTask *t);

/* Per-task dump used by C:taskdump and the watchdog stall dump.
 * name = NULL dumps every live task one line each; a name prints the
 * full saved-frame decode for that task. */
void Task_DiagDump(void *ctx, void (*emit)(void *ctx, const char *line),
                   const char *name, int full);

/* Disable() hold threshold in milliseconds at which irqoff_long counts
 * and kprint warns.  Enable() converts to TSC cycles via the
 * self-calibrated Tickmon_TscHz(). */
#define TASK_IRQOFF_LONG_MS 50

#endif /* UAOS_TASK_H */
