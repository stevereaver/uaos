/* cmd_irqaudit.c — C:irqaudit — Disable/Forbid hold-time audit (UAOS-206)
 *
 *   irqaudit          per-task: current nest levels, total time IF=0,
 *                     longest hold, count of holds > 50 ms, and how many
 *                     times the task was descheduled mid-critical-section
 *                     (the UAOS-169/170/176 bug class — a task that
 *                     sleeps while Disable()'d wedges the machine).
 */

#include "cmd_internal.h"
#include "../exec/task.h"
#include "../dbg/diag.h"

static void hexn(char *line, uint64_t v, int digits)
{
    char h[17];
    for (int i = digits - 1; i >= 0; i--) { h[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
    h[digits] = 0;
    cmd_scat(line, h, CMD_MAX_LINE);
}

void Cmd_Irqaudit(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    char line[CMD_MAX_LINE], num[24];
    uint64_t hz = Tickmon_TscHz();

    PRINT("  #  name                 D  F  if0_tot_ms  if0_max_ms  long  crit-sw");
    for (int i = 0; i < g_task_count; i++) {
        UaosTask *t = &g_tasks[i];
        if (t->tc_State == TASK_REMOVED) continue;
        /* skip tasks that never entered a critical section */
        if (!t->irqoff_ticks && !t->tc_IDNestCnt && !t->irqoff_long
            && !t->switch_while_crit) continue;

        cmd_scopy(line, " ", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)i, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
        int sl = cmd_slen(line); while (sl++ < 4) cmd_scat(line, " ", CMD_MAX_LINE);
        cmd_scat(line, t->ln_Name ? t->ln_Name : "?", CMD_MAX_LINE);
        sl = cmd_slen(line); while (sl++ < 25) cmd_scat(line, " ", CMD_MAX_LINE);
        cmd_sdec(t->tc_IDNestCnt, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
        sl = cmd_slen(line); while (sl++ < 28) cmd_scat(line, " ", CMD_MAX_LINE);
        cmd_sdec(t->tc_TDNestCnt, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
        sl = cmd_slen(line); while (sl++ < 31) cmd_scat(line, " ", CMD_MAX_LINE);

        uint64_t tot = hz ? t->irqoff_ticks * 1000 / hz : 0;
        cmd_uint_to_dec((uint32_t)tot, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
        sl = cmd_slen(line); while (sl++ < 43) cmd_scat(line, " ", CMD_MAX_LINE);

        uint64_t mx = hz ? t->irqoff_max_ticks * 1000 / hz : 0;
        cmd_uint_to_dec((uint32_t)mx, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
        sl = cmd_slen(line); while (sl++ < 55) cmd_scat(line, " ", CMD_MAX_LINE);

        cmd_uint_to_dec(t->irqoff_long, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
        sl = cmd_slen(line); while (sl++ < 61) cmd_scat(line, " ", CMD_MAX_LINE);

        cmd_uint_to_dec(t->switch_while_crit, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
        PRINT(line);
    }
    PRINT("(D=Disable nest  F=Forbid nest  long=holds>50ms  crit-sw=descheduled in crit)");
}
