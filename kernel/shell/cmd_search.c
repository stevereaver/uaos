/* cmd_search.c — C:search — search files for text patterns */

#include "cmd_internal.h"

static int search_ci_eq(unsigned char a, unsigned char b)
{
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'B' && b <= 'Z') b += 32;
    return a == b;
}

static int search_match(const char *needle, int nl,
                        const uint8_t *hay, int hl, int ci)
{
    if (nl == 0) return 1;
    for (int i = 0; i <= hl - nl; i++) {
        int ok = 1;
        for (int j = 0; j < nl; j++) {
            if (ci) {
                if (!search_ci_eq((unsigned char)needle[j], hay[i + j]))
                    { ok = 0; break; }
            } else {
                if ((unsigned char)needle[j] != hay[i + j])
                    { ok = 0; break; }
            }
        }
        if (ok) return 1;
    }
    return 0;
}

static int match_file_pattern(const char *name, const char *pattern)
{
    /* Simple glob: supports * wildcard at start or end */
    if (!pattern || !*pattern) return 1;
    int pl = cmd_slen(pattern);
    int nl = cmd_slen(name);
    if (pattern[0] == '*') {
        const char *suffix = pattern + 1;
        int sl = pl - 1;
        if (sl <= 0) return 1;
        if (nl >= sl) {
            int match = 1;
            for (int i = 0; i < sl; i++) {
                char c = name[nl - sl + i];
                char k = suffix[i];
                if (c >= 'A' && c <= 'Z') c += 32;
                if (k >= 'A' && k <= 'Z') k += 32;
                if (c != k) { match = 0; break; }
            }
            if (match) return 1;
        }
    }
    if (pl > 0 && pattern[pl - 1] == '*') {
        int prefix_len = pl - 1;
        if (prefix_len <= 0) return 1;
        if (nl >= prefix_len) {
            int match = 1;
            for (int i = 0; i < prefix_len; i++) {
                char c = name[i];
                char k = pattern[i];
                if (c >= 'A' && c <= 'Z') c += 32;
                if (k >= 'A' && k <= 'Z') k += 32;
                if (c != k) { match = 0; break; }
            }
            if (match) return 1;
        }
    }
    /* Exact match */
    return cmd_seq_ci(name, pattern);
}

static void search_file(NativeCmdCtx *ctx, const char *path,
                        const char *pattern, int ci, int *hits, int *files)
{
    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ)) return;

    uint32_t pos = 0;
    uint32_t sz = VFS_Size(&fh);
    int line_no = 0;
    int file_hits = 0;

    while (pos < sz) {
        uint8_t buf[CMD_MAX_LINE];
        int col = 0;
        while (pos < sz && col < CMD_MAX_LINE - 1) {
            uint8_t c;
            if (VFS_Read(&fh, &c, 1) == 0) break;
            pos++;
            if (c == '\n') break;
            if (c != '\r') buf[col++] = c;
        }
        buf[col] = '\0';
        line_no++;

        if (search_match(pattern, cmd_slen(pattern), buf, col, ci)) {
            char out[CMD_MAX_LINE];
            out[0] = '\0';
            cmd_scat(out, path, CMD_MAX_LINE);
            cmd_scat(out, ":", CMD_MAX_LINE);
            char lnum[8];
            cmd_uint_to_dec((uint32_t)line_no, lnum, 8);
            cmd_scat(out, lnum, CMD_MAX_LINE);
            cmd_scat(out, ": ", CMD_MAX_LINE);
            cmd_scat(out, (char *)buf, CMD_MAX_LINE);
            PRINT(out);
            file_hits++;
        }
    }

    VFS_Close(&fh);
    if (file_hits) {
        (*files)++;
        *hits += file_hits;
    }
}

static void search_dir(NativeCmdCtx *ctx, const char *path,
                       const char *pattern, int ci, int all,
                       const char *file_pat, int *hits, int *files)
{
    RamFsNode *child = VFS_OpenDir(path);
    if (!child) return;

    while (child) {
        char sub[CMD_MAX_PATH];
        cmd_scopy(sub, path, CMD_MAX_PATH);
        int sl = cmd_slen(sub);
        if (sl > 0 && sub[sl - 1] != ':' && sub[sl - 1] != '/') {
            if (sl < CMD_MAX_PATH - 1) { sub[sl] = '/'; sub[sl + 1] = '\0'; }
        }
        cmd_scat(sub, child->name, CMD_MAX_PATH);

        if (child->type == RAMFS_TYPE_DIR) {
            if (all) search_dir(ctx, sub, pattern, ci, all, file_pat, hits, files);
        } else {
            if (match_file_pattern(child->name, file_pat)) {
                search_file(ctx, sub, pattern, ci, hits, files);
            }
        }
        child = child->next_sibling;
    }
}

void Cmd_Search(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: search [-i] <pattern> [file/dir] [ALL] [FROM <dir>] [FILE <pat>]");
        PRINT("  -i   case-insensitive matching");
        return;
    }

    int ci = 0;
    const char *p = args;
    while (*p == '-') {
        p++;
        while (*p && *p != ' ') {
            if (*p == 'i') ci = 1;
            p++;
        }
        while (*p == ' ') p++;
    }

    int all = cmd_kw_find(args, "ALL");

    /* Parse FROM <dir> */
    char from_dir[CMD_MAX_PATH];
    from_dir[0] = '\0';
    {
        const char *fp = args;
        while (*fp) {
            while (*fp == ' ') fp++;
            if (!*fp) break;
            const char *start = fp;
            while (*fp && *fp != ' ') fp++;
            int len = (int)(fp - start);
            if (len == 4 &&
                ((start[0]=='F'||start[0]=='f') && (start[1]=='R'||start[1]=='r') &&
                 (start[2]=='O'||start[2]=='o') && (start[3]=='M'||start[3]=='m'))) {
                while (*fp == ' ') fp++;
                int i = 0;
                while (*fp && *fp != ' ' && i < CMD_MAX_PATH - 1) from_dir[i++] = *fp++;
                from_dir[i] = '\0';
                break;
            }
        }
    }

    /* Parse FILE <pattern> */
    char file_pat[RAMFS_MAX_NAME];
    file_pat[0] = '\0';
    {
        const char *fp = args;
        while (*fp) {
            while (*fp == ' ') fp++;
            if (!*fp) break;
            const char *start = fp;
            while (*fp && *fp != ' ') fp++;
            int len = (int)(fp - start);
            if (len == 4 &&
                ((start[0]=='F'||start[0]=='f') && (start[1]=='I'||start[1]=='i') &&
                 (start[2]=='L'||start[2]=='l') && (start[3]=='E'||start[3]=='e'))) {
                while (*fp == ' ') fp++;
                int i = 0;
                while (*fp && *fp != ' ' && i < RAMFS_MAX_NAME - 1) file_pat[i++] = *fp++;
                file_pat[i] = '\0';
                break;
            }
        }
    }

    /* Extract pattern and optional target path */
    char pattern[CMD_MAX_LINE];
    int pi = 0;
    while (*p && *p != ' ' && pi < CMD_MAX_LINE - 1)
        pattern[pi++] = *p++;
    pattern[pi] = '\0';
    while (*p == ' ') p++;

    /* Build clean remaining args for path */
    char clean[CMD_MAX_LINE];
    cmd_scopy(clean, p, CMD_MAX_LINE);
    cmd_kw_strip(clean, "ALL", NULL, clean, CMD_MAX_LINE);
    if (from_dir[0]) {
        char fkw[CMD_MAX_PATH + 8];
        fkw[0] = '\0';
        cmd_scat(fkw, "FROM ", CMD_MAX_PATH + 8);
        cmd_scat(fkw, from_dir, CMD_MAX_PATH + 8);
        cmd_kw_strip(clean, fkw, NULL, clean, CMD_MAX_LINE);
    }
    if (file_pat[0]) {
        char fkw[RAMFS_MAX_NAME + 8];
        fkw[0] = '\0';
        cmd_scat(fkw, "FILE ", RAMFS_MAX_NAME + 8);
        cmd_scat(fkw, file_pat, RAMFS_MAX_NAME + 8);
        cmd_kw_strip(clean, fkw, NULL, clean, CMD_MAX_LINE);
    }

    if (!pattern[0]) {
        PRINT("Usage: search [-i] <pattern> [file/dir] [ALL] [FROM <dir>] [FILE <pat>]");
        return;
    }

    char path[CMD_MAX_PATH];
    if (from_dir[0])
        cmd_make_abs(ctx->cwd, from_dir, path, CMD_MAX_PATH);
    else if (clean[0])
        cmd_make_abs(ctx->cwd, clean, path, CMD_MAX_PATH);
    else
        cmd_scopy(path, ctx->cwd, CMD_MAX_PATH);

    int hits = 0, files = 0;

    /* If target is a file, search it directly */
    VfsFile test;
    if (VFS_Open(&test, path, VFS_READ)) {
        VFS_Close(&test);
        search_file(ctx, path, pattern, ci, &hits, &files);
    } else {
        search_dir(ctx, path, pattern, ci, all, file_pat[0] ? file_pat : "", &hits, &files);
    }

    char summary[CMD_MAX_LINE];
    cmd_uint_to_dec((uint32_t)hits, summary, CMD_MAX_LINE);
    cmd_scat(summary, " hits in ", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)files, summary + cmd_slen(summary), CMD_MAX_LINE - cmd_slen(summary));
    cmd_scat(summary, " file(s)", CMD_MAX_LINE);
    PRINT(summary);
}
