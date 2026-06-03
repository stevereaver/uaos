/* cmd_internal.h — shared internals for UAOS C: command implementations
 *
 * Each cmd_*.c file includes this header.  It brings in the NativeCmdCtx
 * type, the VFS/RamFS/BlockDev interfaces, and small inline helpers that
 * duplicate (or replace) the static helpers from shell_win.c.
 *
 * This header is NOT part of the public kernel API; include it only from
 * files inside kernel/shell/.
 */

#ifndef UAOS_CMD_INTERNAL_H
#define UAOS_CMD_INTERNAL_H

#include "native_cmd.h"
#include "../dos/vfs.h"
#include "../dos/ramfs.h"
#include "../dos/blockdev.h"
#include "../dos/partition.h"
#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * String helpers (no libc — freestanding kernel)
 * ------------------------------------------------------------------------- */

static inline int cmd_slen(const char *s)
{
    int n = 0; while (s[n]) n++; return n;
}

static inline void cmd_scopy(char *d, const char *s, int max)
{
    int i = 0;
    while (i < max - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static inline void cmd_scat(char *d, const char *s, int max)
{
    int dl = cmd_slen(d);
    cmd_scopy(d + dl, s, max - dl);
}

static inline int cmd_seq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

static inline int cmd_seq_ci(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) {
        char ac = a[i]; if (ac >= 'A' && ac <= 'Z') ac += 32;
        char bc = b[i]; if (bc >= 'A' && bc <= 'Z') bc += 32;
        if (ac != bc) return 0;
        i++;
    }
    char ac = a[i]; if (ac >= 'A' && ac <= 'Z') ac += 32;
    char bc = b[i]; if (bc >= 'A' && bc <= 'Z') bc += 32;
    return ac == bc;
}

static inline void cmd_uint_to_dec(uint32_t v, char *buf, int max)
{
    char tmp[12]; int i = 0, j = 0;
    if (!v) { buf[j++]='0'; buf[j]=0; return; }
    while (v && i < 11) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i-- && j < max - 1) buf[j++] = tmp[i];
    buf[j] = 0;
}

/* -------------------------------------------------------------------------
 * Path helper — build absolute VFS path from cwd + user argument
 * If arg contains ':' it is already absolute.
 * ------------------------------------------------------------------------- */

#define CMD_MAX_PATH  64
#define CMD_MAX_LINE  96

static inline void cmd_make_abs(const char *cwd, const char *arg,
                                 char *out, int max)
{
    const char *p = arg;
    while (*p && *p != ':') p++;
    if (*p == ':') {
        cmd_scopy(out, arg, max);
        return;
    }
    cmd_scopy(out, cwd, max);
    int cl = cmd_slen(out);
    if (cl > 0 && out[cl-1] != ':' && out[cl-1] != '/') {
        if (cl < max - 1) { out[cl] = '/'; out[cl+1] = '\0'; }
    }
    cmd_scat(out, arg, max);
}

/* Convenience print via ctx */
#define PRINT(msg)  CMD_PRINT(ctx, (msg))

#endif /* UAOS_CMD_INTERNAL_H */
