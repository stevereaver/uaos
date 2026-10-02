/* cmd_watchdog.c — C:watchdog — stall-watchdog control (UAOS-198)
 *
 *   watchdog            status
 *   watchdog MS=n       set no-switch budget in ms (min 100)
 *   watchdog OFF        disable
 *   watchdog TEST       hold Forbid() past the budget — expect a dump
 */

#include "cmd_internal.h"
#include "../dbg/diag.h"
#include "../exec/task.h"

static void wd_emit(void *ctx, const char *line)
{
    CMD_PRINT((NativeCmdCtx *)ctx, line);
}

void Cmd_Watchdog(NativeCmdCtx *ctx, const char *args)
{
    char line[CMD_MAX_LINE], num[24];

    if (!args || !*args) {
        cmd_scopy(line, "watchdog: ", CMD_MAX_LINE);
        cmd_scat(line, Watchdog_Enabled() ? "armed, budget " : "OFF", CMD_MAX_LINE);
        if (Watchdog_Enabled()) {
            cmd_uint_to_dec(Watchdog_Budget(), num, sizeof(num));
            cmd_scat(line, num, CMD_MAX_LINE);
            cmd_scat(line, " ms", CMD_MAX_LINE);
        }
        PRINT(line);
        return;
    }
    if (cmd_kw_find(args, "off")) {
        Watchdog_SetBudget(0);
        PRINT("watchdog: OFF");
        return;
    }
    if (cmd_kw_find(args, "test")) {
        if (!Watchdog_Enabled()) { PRINT("watchdog is OFF — arm it first"); return; }
        PRINT("watchdog: holding Forbid() past budget — watch for dump...");
        Watchdog_Test();
        PRINT("watchdog: released (dump should be in dmesg)");
        return;
    }
    /* MS=n */
    const char *p = args;
    while (*p) {
        while (*p == ' ') p++;
        const char *s = p;
        while (*p && *p != ' ') p++;
        int len = (int)(p - s);
        if (len > 3 && (s[0]=='m'||s[0]=='M') && (s[1]=='s'||s[1]=='S') && s[2]=='=') {
            uint32_t ms = 0;
            const char *q = s + 3;
            while (q < p && *q >= '0' && *q <= '9') { ms = ms*10 + (uint32_t)(*q-'0'); q++; }
            if (ms < 100) { PRINT("watchdog: minimum budget 100 ms"); return; }
            Watchdog_SetBudget(ms);
            cmd_scopy(line, "watchdog: armed, budget ", CMD_MAX_LINE);
            cmd_uint_to_dec(ms, num, sizeof(num));
            cmd_scat(line, num, CMD_MAX_LINE);
            cmd_scat(line, " ms", CMD_MAX_LINE);
            PRINT(line);
            return;
        }
    }
    PRINT("usage: watchdog [MS=n|OFF|TEST]");
}
