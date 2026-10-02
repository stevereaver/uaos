/* cmd_etrace.c — C:etrace — kernel event trace ring (UAOS-209)
 *
 *   etrace                    status: mask, records, dropped
 *   etrace MASK=n             enable classes (bit0 irq,1 sched,2 signal,
 *                             3 dos,4 net — hex ok, e.g. MASK=0x1f)
 *   etrace OFF                mask=0
 *   etrace TAIL [n]           print last n records decoded (default 16)
 *   etrace FILE=path          dump binary ring for tools/etrace_decode.py
 */

#include "cmd_internal.h"
#include "../dbg/diag.h"

static void et_emit(void *ctx, const char *line)
{
    CMD_PRINT((NativeCmdCtx *)ctx, line);
}

void Cmd_Etrace(NativeCmdCtx *ctx, const char *args)
{
    char line[CMD_MAX_LINE], num[24];
    const char *p;
    uint64_t v;

    if ((p = cmd_kv_find(args, "file"))) {
        char path[CMD_MAX_PATH]; int i = 0;
        while (p[i] && p[i] != ' ' && i < CMD_MAX_PATH - 1) { path[i] = p[i]; i++; }
        path[i] = 0;
        if (Etrace_DumpFile(path)) {
            cmd_scopy(line, "etrace: wrote ", CMD_MAX_LINE);
            cmd_scat(line, path, CMD_MAX_LINE);
            PRINT(line);
        } else PRINT("etrace: file write failed");
        return;
    }
    if ((p = cmd_kv_find(args, "mask"))) {
        if (cmd_parse_uint(p, &v)) {
            Etrace_SetMask((uint32_t)v);
            cmd_scopy(line, "etrace: mask=0x", CMD_MAX_LINE);
            char h[12]; int hi = 0, started = 0;
            for (int s = 28; s >= 0; s -= 4) {
                int nib = (int)((v >> s) & 15);
                if (nib || started || s == 0) { h[hi++] = "0123456789abcdef"[nib]; started = 1; }
            }
            h[hi] = 0;
            cmd_scat(line, h, CMD_MAX_LINE);
            PRINT(line);
        } else PRINT("etrace: bad mask");
        return;
    }
    if (args && cmd_kw_find(args, "off")) {
        Etrace_SetMask(0);
        PRINT("etrace: off");
        return;
    }
    if (args && cmd_kw_find(args, "tail")) {
        uint32_t n = 16;
        const char *q = args;
        while (*q) {
            while (*q == ' ') q++;
            if (*q >= '0' && *q <= '9') {
                n = 0;
                while (*q >= '0' && *q <= '9') { n = n * 10 + (uint32_t)(*q - '0'); q++; }
                break;
            }
            while (*q && *q != ' ') q++;
        }
        Etrace_Tail(ctx, et_emit, n);
        return;
    }

    cmd_scopy(line, "etrace: mask=0x", CMD_MAX_LINE);
    char h[12]; int hi = 0, started = 0;
    uint32_t m = Etrace_Mask();
    for (int s = 28; s >= 0; s -= 4) {
        int nib = (int)((m >> s) & 15);
        if (nib || started || s == 0) { h[hi++] = "0123456789abcdef"[nib]; started = 1; }
    }
    h[hi] = 0;
    cmd_scat(line, h, CMD_MAX_LINE);
    cmd_scat(line, " records=", CMD_MAX_LINE);
    cmd_uint_to_dec(Etrace_Count(), num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " dropped=", CMD_MAX_LINE);
    cmd_uint_to_dec(Etrace_Dropped(), num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
    PRINT(line);
    PRINT("usage: etrace [MASK=n|OFF|TAIL n|FILE=path]");
}
