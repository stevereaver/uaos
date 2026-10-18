/* cmd_minject.c — C:minject — synthetic pointer injection (UAOS-319)
 *
 * Drives the same g_mouse + Cursor_Move + EventPump_Wake path a HID
 * report takes, so a window drag can be reproduced from the remote
 * shell while prof/taskstat measure where the time goes.
 *
 *   minject X=x Y=y [L=0|1] [R=0|1]             inject one event
 *   minject DRAG X0=x0 Y0=y0 X1=x1 Y1=y1 [STEPS=n] [MS=n]
 *       left-down at (x0,y0), sweep to (x1,y1) in STEPS moves paced MS
 *       milliseconds apart (default 8 ms — a 125 Hz mouse), left-up.
 */

#include "cmd_internal.h"
#include "../irq/ps2mouse.h"
#include "../display/cursor.h"
#include "../dbg/diag.h"
#include "../exec/task.h"

static void inject(int x, int y, int l, int r)
{
    g_mouse.x = x;
    g_mouse.y = y;
    g_mouse.btn_left  = l;
    g_mouse.btn_right = r;
    Cursor_Move(x, y);
    EventPump_Wake();
}

static int kv_int(const char *args, const char *key, int dflt)
{
    const char *p = cmd_kv_find(args, key);
    uint64_t v;
    if (!p || !cmd_parse_uint(p, &v)) return dflt;
    return (int)v;
}

static void num(char *line, int v)
{
    char b[24];
    cmd_uint_to_dec((uint32_t)v, b, sizeof(b));
    cmd_scat(line, b, CMD_MAX_LINE);
}

void Cmd_MInject(NativeCmdCtx *ctx, const char *args)
{
    char line[CMD_MAX_LINE];

    if (!args || !*args) {
        PRINT("usage: minject X=x Y=y [L=0|1] [R=0|1]");
        PRINT("       minject DRAG X0=x0 Y0=y0 X1=x1 Y1=y1 [STEPS=n] [MS=n]");
        return;
    }

    if (!cmd_kw_find(args, "drag")) {
        int x = kv_int(args, "x", -1);
        int y = kv_int(args, "y", -1);
        if (x < 0 || y < 0) { PRINT("minject: X= and Y= required"); return; }
        inject(x, y, kv_int(args, "l", g_mouse.btn_left),
                   kv_int(args, "r", g_mouse.btn_right));
        cmd_scat(line, "minject: -> (", CMD_MAX_LINE);
        num(line, x); cmd_scat(line, ",", CMD_MAX_LINE);
        num(line, y); cmd_scat(line, ") L=", CMD_MAX_LINE);
        num(line, g_mouse.btn_left); cmd_scat(line, " R=", CMD_MAX_LINE);
        num(line, g_mouse.btn_right);
        PRINT(line);
        return;
    }

    int x0 = kv_int(args, "x0", -1), y0 = kv_int(args, "y0", -1);
    int x1 = kv_int(args, "x1", -1), y1 = kv_int(args, "y1", -1);
    if (x0 < 0 || y0 < 0 || x1 < 0 || y1 < 0) {
        PRINT("minject: DRAG needs X0= Y0= X1= Y1=");
        return;
    }
    int steps = kv_int(args, "steps", 60);
    int ms    = kv_int(args, "ms", 8);
    if (steps < 1) steps = 1;
    if (ms < 0)    ms    = 0;

    uint64_t t0 = diag_rdtsc();
    inject(x0, y0, 1, g_mouse.btn_right);
    for (int i = 1; i <= steps; i++) {
        int x = x0 + (int)((int64_t)(x1 - x0) * i / steps);
        int y = y0 + (int)((int64_t)(y1 - y0) * i / steps);
        inject(x, y, 1, g_mouse.btn_right);
        if (ms) CMD_YIELD(ctx, ms);
    }
    inject(x1, y1, 0, g_mouse.btn_right);
    uint64_t t1 = diag_rdtsc();

    uint64_t hz = Tickmon_TscHz();
    uint64_t us = hz ? (t1 - t0) * 1000000 / hz : 0;
    cmd_scat(line, "minject: drag ", CMD_MAX_LINE);
    num(line, x0); cmd_scat(line, ",", CMD_MAX_LINE);
    num(line, y0); cmd_scat(line, "->", CMD_MAX_LINE);
    num(line, x1); cmd_scat(line, ",", CMD_MAX_LINE);
    num(line, y1); cmd_scat(line, " steps=", CMD_MAX_LINE);
    num(line, steps); cmd_scat(line, " us=", CMD_MAX_LINE);
    num(line, (int)us);
    PRINT(line);
}
