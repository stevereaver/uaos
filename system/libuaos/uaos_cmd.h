/* uaos_cmd.h — shared helpers for UAOS userspace C: command binaries
 *
 * Provides the small set of freestanding utilities that the native x86-64
 * C: commands need: output helpers, path resolution against the task cwd,
 * AmigaDOS pattern matching, and numeric formatting.  These mirror the
 * helpers in kernel/shell/cmd_internal.h but use the INT 0x80 syscall ABI.
 */

#ifndef UAOS_CMD_H
#define UAOS_CMD_H

#include <stdint.h>
#include <stddef.h>
#include "uaos_syscall.h"
#include "uaos_libc.h"

#define UAOS_CMD_PATH_MAX 256
#define UAOS_CMD_LINE_MAX 128

/* -------------------------------------------------------------------------
 * Output helpers
 * ------------------------------------------------------------------------- */
static inline void put_s(const char *s)
{
    uaos_write(1, s, (long)uaos_strlen(s));
}

static inline void put_c(char c)
{
    uaos_write(1, &c, 1);
}

static inline void put_line(const char *s)
{
    put_s(s);
    put_c('\n');
}

/* Reconstruct the raw argument string from argv[1..] by joining tokens with
 * single spaces.  The shell space-splits the command line when building
 * argv, so this reproduces the original argument string faithfully. */
static inline void cmd_build_args(int argc, const char **argv, char *out, int max)
{
    int ai = 0;
    out[0] = '\0';
    for (int i = 1; i < argc; i++) {
        if (i > 1 && ai + 1 < max) out[ai++] = ' ';
        const char *a = argv[i];
        while (*a && ai + 1 < max) out[ai++] = *a++;
    }
    out[ai] = '\0';
}

/* unsigned decimal into buf[max] */
static inline void uint_to_dec(uint32_t v, char *buf, int max)
{
    char tmp[12]; int i = 0, j = 0;
    if (!v) { buf[j++] = '0'; buf[j] = '\0'; return; }
    while (v && i < 11) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i-- && j < max - 1) buf[j++] = tmp[i];
    buf[j] = '\0';
}

static inline void int_to_dec(int32_t v, char *buf, int max)
{
    if (v < 0) { buf[0] = '-'; uint_to_dec((uint32_t)(-v), buf + 1, max - 1); }
    else       { uint_to_dec((uint32_t)v, buf, max); }
}

/* -------------------------------------------------------------------------
 * Path helpers
 *
 * AmigaDOS paths are absolute if they contain a volume/assign prefix
 * (e.g. "C:foo" or "RAM:T/foo").  A bare argument is relative to the
 * calling task's current working directory.
 * ------------------------------------------------------------------------- */

/* Resolve arg against cwd into out[max].  Reads cwd via the getcwd syscall. */
static inline void cmd_make_abs(const char *arg, char *out, int max)
{
    /* If arg contains ':' it is already absolute. */
    const char *p = arg;
    while (*p && *p != ':') p++;
    if (*p == ':') { uaos_strcpy(out, arg); return; }

    char cwd[UAOS_CMD_PATH_MAX];
    long n = uaos_getcwd(cwd, sizeof(cwd));
    if (n <= 0) cwd[0] = '\0';

    size_t i = 0;
    while (i < (size_t)max - 1 && cwd[i]) { out[i] = cwd[i]; i++; }
    if (i > 0 && out[i - 1] != ':' && out[i - 1] != '/' && i + 1 < (size_t)max)
        out[i++] = '/';
    size_t j = 0;
    while (i + 1 < (size_t)max && arg[j]) { out[i] = arg[j]; i++; j++; }
    out[i] = '\0';
}

/* Join a base directory path and a child name into out[max]. */
static inline void cmd_join_path(const char *base, const char *name,
                                 char *out, int max)
{
    size_t i = 0;
    while (i + 1 < (size_t)max && base[i]) { out[i] = base[i]; i++; }
    if (i > 0 && out[i - 1] != ':' && out[i - 1] != '/' && i + 1 < (size_t)max)
        out[i++] = '/';
    size_t j = 0;
    while (i + 1 < (size_t)max && name[j]) { out[i] = name[j]; i++; j++; }
    out[i] = '\0';
}

/* -------------------------------------------------------------------------
 * AmigaDOS-style pattern matching for filenames.
 * Supports:  ?     = any single character
 *            #?    = zero or more characters (AmigaDOS wildcard)
 *            *     = zero or more characters (convenience alias)
 *            %     = empty (NULL) string (AmigaDOS wildcard)
 * Case-insensitive.  Returns 1 if name matches pattern, 0 otherwise.
 * ------------------------------------------------------------------------- */
static inline int cmd_pattern_match(const char *name, const char *pat)
{
    const char *n = name;
    const char *p = pat;
    const char *star_n = NULL;
    const char *star_p = NULL;

    while (*n) {
        char pc = *p;
        char nc = *n;
        if (pc >= 'A' && pc <= 'Z') pc += 32;
        if (nc >= 'A' && nc <= 'Z') nc += 32;

        if (pc == '%') {
            p++;
            continue;
        } else if (pc == '*' || (pc == '#' && p[1] == '?')) {
            if (pc == '#') p++;
            star_p = ++p;
            star_n = n;
            continue;
        } else if (pc == '?') {
            p++; n++;
            continue;
        } else if (pc == nc) {
            p++; n++;
            continue;
        }

        if (star_p) {
            p = star_p;
            n = ++star_n;
            continue;
        }
        return 0;
    }

    while (*p == '*' || (*p == '#' && p[1] == '?') || *p == '%') {
        if (*p == '#') p++;
        p++;
    }
    return *p == '\0';
}

static inline int cmd_has_wildcards(const char *s)
{
    while (*s) {
        if (*s == '?' || *s == '*' || *s == '#' || *s == '%') return 1;
        s++;
    }
    return 0;
}

/* Split an argument that may contain wildcards into directory path and
 * pattern.  If the last component contains wildcards, it becomes the
 * pattern and the preceding part becomes the directory path. */
static inline void cmd_split_path_pat(const char *arg,
                                      char *path_out, char *pat_out)
{
    char abs[UAOS_CMD_PATH_MAX];
    cmd_make_abs(arg, abs, UAOS_CMD_PATH_MAX);

    const char *sep = NULL;
    const char *p = abs;
    while (*p) {
        if (*p == ':' || *p == '/') sep = p;
        p++;
    }

    if (sep && cmd_has_wildcards(sep + 1)) {
        int dir_len = (int)(sep - abs) + 1;
        if (dir_len >= UAOS_CMD_PATH_MAX) dir_len = UAOS_CMD_PATH_MAX - 1;
        uaos_strcpy(path_out, abs);
        path_out[dir_len] = '\0';
        uaos_strcpy(pat_out, sep + 1);
    } else {
        uaos_strcpy(path_out, abs);
        pat_out[0] = '\0';
    }
}

/* -------------------------------------------------------------------------
 * Keyword helpers (case-insensitive whole-word match / strip)
 * ------------------------------------------------------------------------- */
static inline int cmd_kw_find(const char *args, const char *kw)
{
    if (!args || !kw) return 0;
    int kl = (int)uaos_strlen(kw);
    const char *p = args;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ') p++;
        int len = (int)(p - start);
        if (len == kl) {
            int match = 1;
            for (int i = 0; i < kl; i++) {
                char c = start[i]; if (c >= 'A' && c <= 'Z') c += 32;
                char k = kw[i];    if (k >= 'A' && k <= 'Z') k += 32;
                if (c != k) { match = 0; break; }
            }
            if (match) return 1;
        }
    }
    return 0;
}

/* Strip a keyword (and optional following token) from args into out[max].
 * The remaining text is the "cleaned" argument string.  Returns 1 if the
 * keyword was found.  If kw_param is non-NULL, the token after kw is also
 * stripped (used for "KEY value" pairs). */
static inline int cmd_kw_strip(const char *args, const char *kw,
                               const char *kw_param,
                               char *out, int max)
{
    if (!args) { out[0] = '\0'; return 0; }
    int kl = (int)uaos_strlen(kw);
    int pl = kw_param ? (int)uaos_strlen(kw_param) : 0;
    int found = 0, oi = 0;
    const char *p = args;
    while (*p && oi < max - 1) {
        while (*p == ' ') {
            if (oi > 0 && out[oi - 1] != ' ') out[oi++] = ' ';
            p++;
            if (oi >= max - 1) break;
        }
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ') p++;
        int len = (int)(p - start);
        int is_kw = 0;
        if (len == kl) {
            int match = 1;
            for (int i = 0; i < kl; i++) {
                char c = start[i]; if (c >= 'A' && c <= 'Z') c += 32;
                char k = kw[i];    if (k >= 'A' && k <= 'Z') k += 32;
                if (c != k) { match = 0; break; }
            }
            if (match) {
                is_kw = 1; found = 1;
                if (kw_param) {
                    while (*p == ' ') p++;
                    const char *ps = p;
                    while (*p && *p != ' ') p++;
                    if ((int)(p - ps) == pl) {
                        int pm = 1;
                        for (int i = 0; i < pl; i++) {
                            char c = ps[i]; if (c >= 'A' && c <= 'Z') c += 32;
                            char k = kw_param[i]; if (k >= 'A' && k <= 'Z') k += 32;
                            if (c != k) { pm = 0; break; }
                        }
                        if (!pm) p = ps;
                    } else {
                        p = ps;
                    }
                }
            }
        }
        if (!is_kw) {
            if (oi > 0 && out[oi - 1] != ' ' && out[oi - 1] != '\0') out[oi++] = ' ';
            for (int i = 0; i < len && oi < max - 1; i++) out[oi++] = start[i];
        }
    }
    out[oi] = '\0';
    while (oi > 0 && out[oi - 1] == ' ') out[--oi] = '\0';
    return found;
}

/* Format mtime (Unix epoch seconds) as "DD-Mon-YYYY HH:MM" into out[max]. */
static inline void cmd_fmt_mtime(uint32_t ts, char *out, int max)
{
    static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    if (ts == 0) { uaos_strcpy(out, "--/--/--"); return; }
    (void)max;
    /* Days since 1970-01-01 (Thursday).  Compute civil date. */
    uint32_t days = ts / 86400;
    uint32_t secs = ts % 86400;
    int hour = (int)(secs / 3600);
    int min  = (int)((secs / 60) % 60);
    /* day_of_week: 1970-01-01 was Thursday (4) */
    int dow = (int)((4 + days) % 7);
    (void)dow;

    /* Howard Hinnant's civil_from_days */
    int32_t z = (int32_t)days + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t y = (int32_t)(yoe) + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y += 1;

    out[0] = (char)('0' + d / 10);
    out[1] = (char)('0' + d % 10);
    out[2] = '-';
    const char *mc = mon + (m - 1) * 3;
    out[3] = mc[0]; out[4] = mc[1]; out[5] = mc[2];
    out[6] = '-';
    uint32_t yr = (uint32_t)y;
    out[7]  = (char)('0' + (yr / 1000) % 10);
    out[8]  = (char)('0' + (yr / 100)  % 10);
    out[9]  = (char)('0' + (yr / 10)   % 10);
    out[10] = (char)('0' + yr          % 10);
    out[11] = ' ';
    out[12] = (char)('0' + hour / 10);
    out[13] = (char)('0' + hour % 10);
    out[14] = ':';
    out[15] = (char)('0' + min / 10);
    out[16] = (char)('0' + min % 10);
    out[17] = '\0';
}

#endif /* UAOS_CMD_H */
