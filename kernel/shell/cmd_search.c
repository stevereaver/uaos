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
                       const char *pattern, int ci, int *hits, int *files)
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
            search_dir(ctx, sub, pattern, ci, hits, files);
        } else {
            search_file(ctx, sub, pattern, ci, hits, files);
        }
        child = child->next_sibling;
    }
}

void Cmd_Search(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: search [-i] <pattern> [file/dir]");
        PRINT("  -i   case-insensitive matching");
        return;
    }

    int ci = 0;
    while (*args == '-') {
        args++;
        while (*args && *args != ' ') {
            if (*args == 'i') ci = 1;
            args++;
        }
        while (*args == ' ') args++;
    }

    char pattern[CMD_MAX_LINE];
    int pi = 0;
    while (*args && *args != ' ' && pi < CMD_MAX_LINE - 1)
        pattern[pi++] = *args++;
    pattern[pi] = '\0';
    while (*args == ' ') args++;

    if (!pattern[0]) {
        PRINT("Usage: search [-i] <pattern> [file/dir]");
        return;
    }

    char path[CMD_MAX_PATH];
    if (*args)
        cmd_make_abs(ctx->cwd, args, path, CMD_MAX_PATH);
    else
        cmd_scopy(path, ctx->cwd, CMD_MAX_PATH);

    int hits = 0, files = 0;

    /* If target is a file, search it directly */
    VfsFile test;
    if (VFS_Open(&test, path, VFS_READ)) {
        VFS_Close(&test);
        search_file(ctx, path, pattern, ci, &hits, &files);
    } else {
        search_dir(ctx, path, pattern, ci, &hits, &files);
    }

    char summary[CMD_MAX_LINE];
    cmd_uint_to_dec((uint32_t)hits, summary, CMD_MAX_LINE);
    cmd_scat(summary, " hits in ", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)files, summary + cmd_slen(summary), CMD_MAX_LINE - cmd_slen(summary));
    cmd_scat(summary, " file(s)", CMD_MAX_LINE);
    PRINT(summary);
}
