/* cmd_prof.c — C:prof — PIT-sampled RIP profiler (UAOS-210)
 *
 *   prof START            begin sampling outermost ISR frame RIP
 *   prof STOP             stop
 *   prof REPORT [n]       top-n hot RIPs (default 10) — symbolize
 *                         with tools/symbolize.sh
 *   prof FILE=path        dump "rip taskidx count" lines for
 *                         tools/prof_report.py
 */

#include "cmd_internal.h"
#include "../dbg/diag.h"

static void pf_emit(void *ctx, const char *line)
{
    CMD_PRINT((NativeCmdCtx *)ctx, line);
}

void Cmd_Prof(NativeCmdCtx *ctx, const char *args)
{
    char line[CMD_MAX_LINE], num[24];
    const char *p;

    if ((p = cmd_kv_find(args, "file"))) {
        char path[CMD_MAX_PATH]; int i = 0;
        while (p[i] && p[i] != ' ' && i < CMD_MAX_PATH - 1) { path[i] = p[i]; i++; }
        path[i] = 0;
        if (Prof_DumpFile(path)) { cmd_scopy(line, "prof: wrote ", CMD_MAX_LINE); cmd_scat(line, path, CMD_MAX_LINE); PRINT(line); }
        else PRINT("prof: file write failed");
        return;
    }
    if (args && cmd_kw_find(args, "start")) {
        Prof_Start();
        PRINT("prof: sampling on every PIT tick");
        return;
    }
    if (args && cmd_kw_find(args, "stop")) {
        Prof_Stop();
        PRINT("prof: stopped");
        return;
    }
    if (args && cmd_kw_find(args, "report")) {
        int top = 10;
        const char *q = args;
        while (*q) {
            while (*q == ' ') q++;
            if (*q >= '0' && *q <= '9') {
                top = 0;
                while (*q >= '0' && *q <= '9') { top = top * 10 + (*q - '0'); q++; }
                break;
            }
            while (*q && *q != ' ') q++;
        }
        cmd_scopy(line, "prof: top ", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)top, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, " of ", CMD_MAX_LINE);
        cmd_uint_to_dec(Prof_SampleCount(), num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, " samples (symbolize RIPs via tools/symbolize.sh)", CMD_MAX_LINE);
        PRINT(line);
        Prof_Report(ctx, pf_emit, top);
        return;
    }

    cmd_scopy(line, "prof: ", CMD_MAX_LINE);
    cmd_scat(line, Prof_Running() ? "RUNNING" : "stopped", CMD_MAX_LINE);
    cmd_scat(line, " samples=", CMD_MAX_LINE);
    cmd_uint_to_dec(Prof_SampleCount(), num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
    PRINT(line);
    PRINT("usage: prof START|STOP|REPORT [n]|FILE=path");
}
