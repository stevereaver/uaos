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
#include "cmd_template.h"
#include "../dos/vfs.h"
#include "../dos/ramfs.h"
#include "../dos/amiga_dos_types.h"
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
 * AmigaDOS-style rules:
 *   NAME:...       absolute volume reference
 *   :              root of current volume
 *   :dir           relative to root of current volume
 *   /              parent directory (one level up)
 *   //             two levels up
 *   /foo           parent directory, then into "foo"
 * ------------------------------------------------------------------------- */

#define CMD_MAX_PATH  64
#define CMD_MAX_LINE  96

static inline void cmd_make_abs(const char *cwd, const char *arg,
                                 char *out, int max)
{
    if (!arg || !*arg) {
        cmd_scopy(out, cwd, max);
        return;
    }

    /* Absolute volume reference: NAME:... (NAME is non-empty) */
    if (arg[0] != ':' && arg[0] != '/') {
        const char *p = arg;
        while (*p && *p != ':') p++;
        if (*p == ':') {
            cmd_scopy(out, arg, max);
            return;
        }
    }

    /* Root-relative on current volume: ":" or ":dir" */
    if (arg[0] == ':') {
        const char *colon = cwd;
        while (*colon && *colon != ':') colon++;
        int vol_len = (int)(colon - cwd) + 1; /* include ':' */
        if (vol_len >= max) vol_len = max - 1;
        int i = 0;
        for (; i < vol_len && i < max - 1; i++) out[i] = cwd[i];
        out[i] = '\0';
        cmd_scat(out, arg + 1, max);
        return;
    }

    /* Parent navigation: leading "/" goes up N levels, then appends. */
    if (arg[0] == '/') {
        const char *colon = cwd;
        while (*colon && *colon != ':') colon++;
        int vol_len = (int)(colon - cwd) + 1; /* include ':' */

        int up = 0;
        while (arg[up] == '/') up++;

        cmd_scopy(out, cwd, max);
        int len = cmd_slen(out);

        for (int i = 0; i < up && len > vol_len; i++) {
            while (len > vol_len && out[len - 1] == '/')
                out[--len] = '\0';
            while (len > vol_len && out[len - 1] != '/')
                out[--len] = '\0';
            while (len > vol_len && out[len - 1] == '/')
                out[--len] = '\0';
        }

        const char *rest = arg + up;
        if (*rest) {
            if (len > 0 && out[len - 1] != ':' && len < max - 1) {
                out[len] = '/';
                out[len + 1] = '\0';
                len++;
            }
            cmd_scat(out, rest, max);
        }
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

/* -------------------------------------------------------------------------
 * Shared helpers for flag parsing and interactive prompts
 * ------------------------------------------------------------------------- */

/* Return 1 if keyword appears as a whole word in args (case-insensitive). */
static inline int cmd_kw_find(const char *args, const char *kw)
{
    if (!args || !kw) return 0;
    int kl = cmd_slen(kw);
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
 * The remaining text is the "cleaned" argument string.
 * If kw_param is non-NULL, the token immediately after kw is also stripped.
 * Returns 1 if the keyword was found. */
static inline int cmd_kw_strip(const char *args, const char *kw,
                                const char *kw_param,
                                char *out, int max)
{
    if (!args) { out[0] = '\0'; return 0; }
    int kl = cmd_slen(kw);
    int pl = kw_param ? cmd_slen(kw_param) : 0;
    int found = 0;
    int oi = 0;
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
                /* Skip parameter token if requested */
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
                        if (!pm) p = ps; /* rewind if param didn't match */
                    } else {
                        p = ps; /* rewind */
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

/* Blocking yes/no prompt. Returns 1 for yes, 0 for no. */
static inline int cmd_prompt_yn(NativeCmdCtx *ctx, const char *msg)
{
    char line[CMD_MAX_LINE];
    cmd_scopy(line, msg, CMD_MAX_LINE);
    cmd_scat(line, " (y/n)? ", CMD_MAX_LINE);
    PRINT(line);
    if (!ctx->read_line) return 0;
    char buf[8];
    int n = ctx->read_line(ctx->shell_extra, buf, sizeof(buf));
    if (n > 0 && (buf[0] == 'y' || buf[0] == 'Y')) return 1;
    return 0;
}

/* Copy a single file from src to dst. Returns bytes copied or -1 on error. */
static inline int cmd_copy_file(const char *src, const char *dst)
{
    VfsFile fsrc;
    if (!VFS_Open(&fsrc, src, VFS_READ)) return -1;
    VfsFile fdst;
    if (!VFS_Open(&fdst, dst, VFS_WRITE | VFS_CREATE)) {
        VFS_Close(&fsrc);
        return -1;
    }
    char buf[256];
    int total = 0;
    while (1) {
        int n = (int)VFS_Read(&fsrc, (uint8_t *)buf, 256);
        if (n <= 0) break;
        VFS_Write(&fdst, (const uint8_t *)buf, (uint32_t)n);
        total += n;
    }
    VFS_Close(&fsrc);
    VFS_Close(&fdst);
    return total;
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
            /* % matches the empty (NULL) string */
            p++;
            continue;
        } else if (pc == '*' || (pc == '#' && p[1] == '?')) {
            /* #? consumes two chars; * consumes one */
            if (pc == '#') p++;
            star_p = ++p;
            star_n = n;
            continue;
        } else if (pc == '?') {
            p++;
            n++;
            continue;
        } else if (pc == nc) {
            p++;
            n++;
            continue;
        }

        if (star_p) {
            p = star_p;
            n = ++star_n;
            continue;
        }
        return 0;
    }

    /* Consume trailing #? or * or % */
    while (*p == '*' || (*p == '#' && p[1] == '?') || *p == '%') {
        if (*p == '#') p++;
        p++;
    }
    return *p == '\0';
}

/* Return 1 if s contains any wildcard characters (? * # %) */
static inline int cmd_has_wildcards(const char *s)
{
    while (*s) {
        if (*s == '?' || *s == '*' || *s == '#' || *s == '%') return 1;
        s++;
    }
    return 0;
}

/* Split an argument that may contain wildcards into directory path and pattern.
 * If the last component contains wildcards, it becomes the pattern and the
 * preceding part becomes the directory path.  Otherwise pattern is empty.
 * path_out and pat_out must each be at least CMD_MAX_PATH bytes.
 */
static inline void cmd_split_path_pat(const char *cwd, const char *arg,
                                        char *path_out, char *pat_out)
{
    char abs[CMD_MAX_PATH];
    cmd_make_abs(cwd, arg, abs, CMD_MAX_PATH);

    /* Find last separator */
    const char *sep = NULL;
    const char *p = abs;
    while (*p) {
        if (*p == ':' || *p == '/') sep = p;
        p++;
    }

    if (sep && cmd_has_wildcards(sep + 1)) {
        int dir_len = (int)(sep - abs) + 1;
        if (dir_len >= CMD_MAX_PATH) dir_len = CMD_MAX_PATH - 1;
        cmd_scopy(path_out, abs, CMD_MAX_PATH);
        path_out[dir_len] = '\0';
        cmd_scopy(pat_out, sep + 1, CMD_MAX_PATH);
    } else {
        cmd_scopy(path_out, abs, CMD_MAX_PATH);
        pat_out[0] = '\0';
    }
}

/* Recursive delete of a directory tree.
 * If pat is non-NULL and non-empty, only delete entries whose names match pat.
 * Returns 0 on success. */
static inline int cmd_delete_recursive(const char *path, int force, const char *pat)
{
    RamFsNode *node = VFS_ResolveDir(path);
    if (!node) {
        /* Try as file */
        VfsFile test;
        if (VFS_Open(&test, path, VFS_READ)) {
            VFS_Close(&test);
            if (pat && pat[0]) {
                const char *name = path;
                const char *tmp = path;
                while (*tmp) { if (*tmp == ':' || *tmp == '/') name = tmp + 1; tmp++; }
                if (!cmd_pattern_match(name, pat)) return -1;
            }
            int rc = VFS_Delete(path);
            if (rc == -4 && force) {
                uint16_t p = VFS_GetProtection(path);
                VFS_SetProtection(path, p & ~FIBF_DELETE);
                rc = VFS_Delete(path);
            }
            return rc;
        }
        return -1;
    }
    /* Directory: delete matching children first */
    RamFsNode *child = node->first_child;
    while (child) {
        RamFsNode *next = child->next_sibling;
        if (!pat || !pat[0] || cmd_pattern_match(child->name, pat)) {
            char sub[CMD_MAX_PATH];
            cmd_scopy(sub, path, CMD_MAX_PATH);
            int sl = cmd_slen(sub);
            if (sl > 0 && sub[sl - 1] != ':' && sub[sl - 1] != '/') {
                if (sl < CMD_MAX_PATH - 1) { sub[sl] = '/'; sub[sl + 1] = '\0'; }
            }
            cmd_scat(sub, child->name, CMD_MAX_PATH);
            cmd_delete_recursive(sub, force, pat);
        }
        child = next;
    }
    /* Only delete the directory itself if no pattern is filtering */
    if (!pat || !pat[0]) return VFS_Delete(path);
    return 0;
}

#endif /* UAOS_CMD_INTERNAL_H */
