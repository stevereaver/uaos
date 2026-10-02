/* watchdog.c — kernel stall watchdog (UAOS-198)
 *
 * Catches the failure mode the exception path cannot: a scheduler or
 * interrupt stall that produces NO fault.  On machines without serial
 * (MacBookPro4,1) such a hang is otherwise a silent frozen screen.
 *
 * Two independent tripwires, both serviced from IRQ context:
 *
 *  1. PIT tick: if PIT is firing but g_ctx_switches has not advanced for
 *     longer than the budget — while >1 task is runnable — the scheduler
 *     is wedged (e.g. a task parked holding a critical section, or a
 *     corrupted ready queue).  A ≤1-runnable-task system is idle by
 *     definition, not stalled.
 *
 *  2. RTC second: if the RTC heartbeat IRQ arrives but g_pit_ticks has
 *     not advanced at all in a whole second, the PIT path itself is dead
 *     (masked GSI, broken EOI, storm starvation).
 *
 * On a trip, the dump goes through kprint (UART + klog ring) AND
 * Dbgcon_Resume() so it is visible on the framebuffer even post-LoadWB.
 *
 * Config: kernel cmdline "watchdog=<ms>" (0 disables; default 5000 ms)
 *         or at runtime via "C:watchdog MS=n|OFF|STATUS|TEST".
 */

#include "diag.h"
#include "../exec/task.h"
#include "../irq/idt.h"
#include "../boot/kprint.h"

extern volatile uint64_t g_pit_ticks;   /* 100 Hz — uaos_kernel_main.c */
extern void Dbgcon_Resume(void);

#define WATCHDOG_DEFAULT_MS 5000
#define WATCHDOG_REDUMP_TICKS 500       /* don't re-dump faster than 5 s */

static uint32_t          g_wd_budget_ms   = WATCHDOG_DEFAULT_MS;
static volatile int      g_wd_stalled     = 0;
static volatile uint64_t g_wd_last_switch;
static volatile uint64_t g_wd_last_prog;  /* g_pit_ticks at last progress */
static volatile uint64_t g_wd_tick_at_rtc;/* g_pit_ticks at last RTC sec */
static volatile uint64_t g_wd_last_dump;

void Watchdog_SetBudget(uint32_t ms)
{
    g_wd_budget_ms = ms;
    g_wd_last_prog = g_pit_ticks;
    g_wd_last_switch = g_ctx_switches;
    g_wd_stalled = 0;
}

uint32_t Watchdog_Budget(void) { return g_wd_budget_ms; }
int      Watchdog_Enabled(void) { return g_wd_budget_ms != 0; }

/* kprint-based emit so the same dump code serves UART/serial/klog. */
static void wd_kprint_emit(void *ctx, const char *line)
{
    (void)ctx;
    kprint(line);
    kprint("\n");
}

void Watchdog_Dump(void *ctx, DiagEmitFn emit)
{
    DiagLine l;

    dl_reset(&l);
    dl_add(&l, "================== WATCHDOG STALL ==================");
    dl_emit(&l, ctx, emit);

    dl_add(&l, "pit_ticks="); dl_dec(&l, g_pit_ticks);
    dl_add(&l, " ctx_switches="); dl_dec(&l, g_ctx_switches);
    dl_add(&l, " runnable="); dl_dec(&l, (uint64_t)Task_RunnableCount());
    dl_add(&l, " irq_depth="); dl_dec(&l, (uint64_t)(uint32_t)g_irq_depth);
    dl_emit(&l, ctx, emit);

    /* Where was the CPU when we were interrupted?  The stashed outermost
     * ISR frame is THIS IRQ's frame — its RIP is the stalled code. */
    IsrFrame *f = IDT_LastIsrFrame();
    if (f) {
        dl_add(&l, "interrupted rip="); dl_hex(&l, f->rip);
        dl_add(&l, " rsp="); dl_hex(&l, f->rsp);
        dl_add(&l, " rflags="); dl_hex(&l, f->rflags);
        dl_emit(&l, ctx, emit);
    }

    UaosTask *cur = Task_Current();
    dl_add(&l, "current task: ");
    dl_add(&l, (cur && cur->ln_Name) ? cur->ln_Name : "(none)");
    if (cur) {
        dl_add(&l, " state="); dl_dec(&l, cur->tc_State);
        dl_add(&l, " IDNest="); dl_sdec(&l, cur->tc_IDNestCnt);
        dl_add(&l, " TDNest="); dl_sdec(&l, cur->tc_TDNestCnt);
        dl_add(&l, " saved_rip="); dl_hex(&l, cur->native_rsp
            ? ((const uint64_t *)(uintptr_t)cur->native_rsp)[17] : 0);
    }
    dl_emit(&l, ctx, emit);

    Task_DiagDump(ctx, emit, NULL, 0);

    dl_add(&l, "====================================================");
    dl_emit(&l, ctx, emit);
}

static void watchdog_trip(const char *why)
{
    uint64_t now = g_pit_ticks;
    if (now - g_wd_last_dump < WATCHDOG_REDUMP_TICKS)
        return;
    g_wd_last_dump = now;

    kprint("\n[WATCHDOG] stall detected: ");
    kprint(why);
    kprint("\n");

    /* On serial-less machines the framebuffer is the only output —
     * repaint the debug console so the dump is visible there too. */
    Dbgcon_Resume();
    Watchdog_Dump(NULL, wd_kprint_emit);
    g_wd_stalled = 1;
}

void Watchdog_Tick(void)
{
    if (!g_wd_budget_ms) return;

    uint64_t now = g_pit_ticks;

    if (g_ctx_switches != g_wd_last_switch) {
        g_wd_last_switch = g_ctx_switches;
        g_wd_last_prog = now;
        if (g_wd_stalled) {
            g_wd_stalled = 0;
            kprint("[WATCHDOG] scheduler resumed — stall cleared\n");
        }
        return;
    }

    /* Idle system: nothing to dispatch is not a stall. */
    if (Task_RunnableCount() <= 1) {
        g_wd_last_prog = now;
        if (g_wd_stalled) g_wd_stalled = 0;
        return;
    }

    /* Budget in ms -> ticks (PIT = 100 Hz, 10 ms per tick) */
    uint64_t budget_ticks = (g_wd_budget_ms + 9) / 10;
    if (now - g_wd_last_prog > budget_ticks)
        watchdog_trip("no context switch within budget");
}

void Watchdog_RtcSecond(void)
{
    if (!g_wd_budget_ms) return;
    /* A whole second elapsed with zero PIT ticks => tick IRQ is dead. */
    if (g_pit_ticks == g_wd_tick_at_rtc)
        watchdog_trip("PIT tick stopped (RTC still firing)");
    g_wd_tick_at_rtc = g_pit_ticks;
}

/* Artificial stall for self-test: hold Forbid() across the budget so PIT
 * keeps firing but no reschedule is allowed — exactly the signature the
 * watchdog watches for.  (An IF=0 hold can't be tested this way: with
 * IF=0 the PIT IRQ never reaches Watchdog_Tick, which is also the real
 * limitation — the RTC-second tripwire covers PIT-dead with IF=1 only.) */
void Watchdog_Test(void)
{
    /* Hold Forbid() across the budget in the CALLING task — PIT ticks
     * keep arriving (IF=1) but do_schedule is suppressed, so the watchdog
     * sees >1 runnable task and zero switches => trips with our task's
     * state front and centre in the dump. */
    Forbid();
    uint64_t end = g_pit_ticks + (g_wd_budget_ms / 10) + 200;
    while (g_pit_ticks < end) {
        __asm__ volatile ("pause" ::: "memory");
        if (g_wd_stalled) break;   /* watchdog already fired — done */
    }
    Permit();
}
