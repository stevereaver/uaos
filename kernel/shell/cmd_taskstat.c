/* cmd_taskstat.c — C:taskstat — per-task CPU + context-switch accounting
 * (UAOS-197)
 *
 *   taskstat            sample over a 1s window, print per-task share
 *   taskstat <sec>      sample over <sec> seconds
 *   taskstat NOW        cumulative counters, no sampling window
 *
 * cpu_ticks are charged in do_schedule — 100 PIT ticks per second of
 * wall time.  "hung or spinning" is a one-command question now.
 */

#include "cmd_internal.h"
#include "../exec/task.h"
#include "../irq/irq.h"

extern volatile uint64_t g_pit_ticks;

typedef struct { uint64_t cpu; uint32_t sw; uint64_t irqoff; } AcctSnap;

static void u64a(char *line, uint64_t v, int w)
{
    char num[24]; cmd_uint_to_dec((uint32_t)v, num, sizeof(num));
    int nd = cmd_slen(num);
    for (int i = 0; i < w - nd; i++) cmd_scat(line, " ", CMD_MAX_LINE);
    cmd_scat(line, num, CMD_MAX_LINE);
}

void Cmd_Taskstat(NativeCmdCtx *ctx, const char *args)
{
    static AcctSnap before[MAX_TASKS];
    static AcctSnap after[MAX_TASKS];
    int now = 0;
    uint32_t secs = 1;

    if (args && *args) {
        if (cmd_seq_ci(args, "now")) {
            now = 1;
        } else if (*args >= '0' && *args <= '9') {
            uint32_t n = 0;
            const char *p = args;
            while (*p >= '0' && *p <= '9') { n = n * 10 + (uint32_t)(*p - '0'); p++; }
            if (n >= 1 && n <= 60) secs = n;
        } else {
            PRINT("usage: taskstat [<sec>|NOW]");
            return;
        }
    }

    uint64_t t0 = g_pit_ticks;
    for (int i = 0; i < g_task_count; i++) {
        before[i].cpu = g_tasks[i].cpu_ticks;
        before[i].sw  = g_tasks[i].ctx_switches;
        before[i].irqoff = g_tasks[i].irqoff_ticks;
    }

    if (!now)
        for (uint32_t i = 0; i < secs * 10; i++)
            CMD_YIELD(ctx, 100);

    uint64_t dt = g_pit_ticks - t0;
    if (!dt) dt = 1;

    PRINT("  #  name                 state    cpu%   sw/s   irqoff_ms  wait  pri");
    char line[CMD_MAX_LINE], num[24];
    uint64_t busy_total = 0;

    for (int i = 0; i < g_task_count; i++) {
        UaosTask *t = &g_tasks[i];
        if (t->tc_State == TASK_REMOVED) continue;
        after[i].cpu = t->cpu_ticks;
        after[i].sw  = t->ctx_switches;
        after[i].irqoff = t->irqoff_ticks;

        uint64_t dcpu = now ? t->cpu_ticks : after[i].cpu - before[i].cpu;
        uint64_t dsw  = now ? t->ctx_switches : after[i].sw - before[i].sw;
        busy_total += dcpu;

        cmd_scopy(line, " ", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)i, num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
        int sl = cmd_slen(line); while (sl++ < 4) cmd_scat(line, " ", CMD_MAX_LINE);

        cmd_scat(line, t->ln_Name ? t->ln_Name : "?", CMD_MAX_LINE);
        sl = cmd_slen(line); while (sl++ < 25) cmd_scat(line, " ", CMD_MAX_LINE);

        static const char *stn[] = { "wait", "rdy", "run", "gone" };
        cmd_scat(line, stn[t->tc_State & 3], CMD_MAX_LINE);
        sl = cmd_slen(line); while (sl++ < 34) cmd_scat(line, " ", CMD_MAX_LINE);

        /* cpu% over window: delta_ticks * 100 / window ticks */
        uint32_t pct = now ? 0 : (uint32_t)(dcpu * 100 / dt);
        cmd_uint_to_dec(pct, num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE); cmd_scat(line, now ? "(cum)" : "%", CMD_MAX_LINE);
        sl = cmd_slen(line); while (sl++ < 41) cmd_scat(line, " ", CMD_MAX_LINE);

        cmd_uint_to_dec((uint32_t)(now ? dsw : dsw * 100 / dt), num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
        sl = cmd_slen(line); while (sl++ < 49) cmd_scat(line, " ", CMD_MAX_LINE);

        /* irqoff in ms via self-calibrated TSC */
        {
            extern uint64_t Tickmon_TscHz(void);
            uint64_t hz = Tickmon_TscHz();
            uint64_t dcyc = t->irqoff_ticks - before[i].irqoff;
            uint64_t ms = hz ? dcyc * 1000 / hz : dcyc;
            if (now) ms = hz ? t->irqoff_ticks * 1000 / hz : t->irqoff_ticks;
            cmd_uint_to_dec((uint32_t)ms, num, sizeof(num));
            cmd_scat(line, num, CMD_MAX_LINE);
        }
        sl = cmd_slen(line); while (sl++ < 61) cmd_scat(line, " ", CMD_MAX_LINE);

        if (t->tc_SigWait) {
            cmd_scopy(num, "0x", sizeof(num));
            char h[12]; int hi = 0;
            uint32_t v = t->tc_SigWait;
            int started = 0;
            for (int s = 28; s >= 0; s -= 4) {
                int nib = (v >> s) & 15;
                if (nib || started || s == 0) {
                    h[hi++] = "0123456789abcdef"[nib];
                    started = 1;
                }
            }
            h[hi] = 0;
            cmd_scat(num, h, sizeof(num));
            cmd_scat(line, num, CMD_MAX_LINE);
        } else cmd_scat(line, "-", CMD_MAX_LINE);
        sl = cmd_slen(line); while (sl++ < 68) cmd_scat(line, " ", CMD_MAX_LINE);

        cmd_sdec((int32_t)t->ln_Pri, num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
        PRINT(line);
    }

    cmd_scopy(line, "window: ", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)dt, num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " ticks, busy-total ", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)busy_total, num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " (idle share = window - busy)", CMD_MAX_LINE);
    PRINT(line);
}
