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
#define SIGB_BREAKF    8

#define SIGF_ABORT     (1U << SIGB_ABORT)
#define SIGF_CHILD     (1U << SIGB_CHILD)
#define SIGF_BLIT      (1U << SIGB_BLIT)
#define SIGF_SINGLE    (1U << SIGB_SINGLE)
#define SIGF_INTUITION (1U << SIGB_INTUITION)
#define SIGF_NET       (1U << SIGB_NET)
#define SIGF_BREAKF    (1U << SIGB_BREAKF)

/* -------------------------------------------------------------------------
 * Task type
 * ------------------------------------------------------------------------- */
typedef enum {
    TASK_TYPE_NATIVE = 0,
    TASK_TYPE_M68K   = 1,
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

    /* M68k guest fields */
    uint32_t m68k_task_struct;    /* guest RAM address of AmigaOS Task struct */
    uint32_t m68k_context_size;
    void    *m68k_context_buf;    /* host buffer for m68k_get_context/set_context */
    uint8_t *m68k_ram;            /* per-task guest RAM (NULL for native tasks) */
    uint32_t m68k_entry;          /* guest PC entry point */
    uint32_t m68k_stack_top;      /* guest SP */
    uint32_t m68k_bin_size;       /* size of loaded binary */
    int      m68k_initial_cycles; /* saved m68ki_initial_cycles */
    int      m68k_remaining_cycles; /* saved m68ki_remaining_cycles */
    uint8_t  m68k_halted;       /* set when dos_Exit called */
    void    *m68k_print_fn;       /* GluePrintFn for output */

    /* Scheduling */
    uint32_t time_slice_ticks;
    uint32_t ticks_remaining;
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

/* Create an M68k guest task (native wrapper that runs a binary).
 * Returns task pointer or NULL. */
UaosTask *Task_CreateM68k(const char *name, int8_t pri,
                           const uint8_t *binary, uint32_t bin_size,
                           const char **argv,
                           void (*print_fn)(const char *));

/* Current task yields CPU voluntarily */
void Task_Yield(void);

/* Exit current task */
void Task_Exit(void) __attribute__((noreturn));

/* Called from timer ISR when preemption is allowed */
void Task_ScheduleFromIRQ(void);

/* Get currently running task */
UaosTask *Task_Current(void);

/* Idle task entry (system loop) */
void Task_IdleEntry(void *arg);

/* Assembly: switch from current task to next task */
void Task_SwitchContext(UaosTask *old_task, UaosTask *new_task);

/* Assembly: return to a new task's initial entry point */
void Task_RunNew(UaosTask *task);

/* Start the first ready task (called once at boot with interrupts off) */
void Task_StartFirst(void);

/* Global used by assembly ISR to perform context switch */
extern UaosTask *Task_SwitchNext;
extern UaosTask *Task_SwitchPrev;

/* Test helper: spawn UART-printing tasks */
void Task_TestSpawn(void);

#endif /* UAOS_TASK_H */
