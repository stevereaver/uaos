/* cmd_copy.c — C:copy — copy a file or directory */

#include "cmd_internal.h"

static void copy_one(NativeCmdCtx *ctx, const char *src, const char *dst,
                     int clone, int com, int quiet)
{
    int rc = cmd_copy_file(src, dst);
    if (rc < 0) {
        if (!quiet) {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "Failed to copy: ", CMD_MAX_LINE);
            cmd_scat(msg, src, CMD_MAX_LINE);
            PRINT(msg);
        }
        return;
    }

    if (clone) {
        uint16_t prot = VFS_GetProtection(src);
        VFS_SetProtection(dst, prot);
        uint8_t attrs = VFS_GetAttrs(src);
        VFS_SetAttrs(dst, attrs);
    }

    if (com) {
        char comment[64];
        if (VFS_GetComment(src, comment, 64) == 0) {
            VFS_SetComment(dst, comment);
        }
    }

    if (!quiet) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "Copied ", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)rc, msg + cmd_slen(msg), 12);
        cmd_scat(msg, " bytes", CMD_MAX_LINE);
        PRINT(msg);
    }
}

static void copy_dir(NativeCmdCtx *ctx, const char *src, const char *dst,
                     int clone, int com, int quiet, const char *pat)
{
    /* Create destination directory */
    VFS_MkDir(dst);

    RamFsNode *dir = VFS_ResolveDir(src);
    if (!dir) return;

    RamFsNode *child = dir->first_child;
    while (child) {
        int match = !pat || !pat[0] || cmd_pattern_match(child->name, pat);

        char ssub[CMD_MAX_PATH], dsub[CMD_MAX_PATH];
        cmd_scopy(ssub, src, CMD_MAX_PATH);
        cmd_scopy(dsub, dst, CMD_MAX_PATH);
        int sl = cmd_slen(ssub);
        int dl = cmd_slen(dsub);
        if (sl > 0 && ssub[sl - 1] != ':' && ssub[sl - 1] != '/') {
            if (sl < CMD_MAX_PATH - 1) { ssub[sl] = '/'; ssub[sl + 1] = '\0'; }
        }
        if (dl > 0 && dsub[dl - 1] != ':' && dsub[dl - 1] != '/') {
            if (dl < CMD_MAX_PATH - 1) { dsub[dl] = '/'; dsub[dl + 1] = '\0'; }
        }
        cmd_scat(ssub, child->name, CMD_MAX_PATH);
        cmd_scat(dsub, child->name, CMD_MAX_PATH);

        if (child->type == RAMFS_TYPE_DIR) {
            if (!pat || !pat[0]) {
                copy_dir(ctx, ssub, dsub, clone, com, quiet, pat);
            } else if (match) {
                /* Pattern matches directory name: recurse and copy all inside */
                copy_dir(ctx, ssub, dsub, clone, com, quiet, NULL);
            } else {
                /* Pattern doesn't match dir: still recurse for ALL-like behaviour */
                copy_dir(ctx, ssub, dsub, clone, com, quiet, pat);
            }
        } else if (match) {
            copy_one(ctx, ssub, dsub, clone, com, quiet);
        }
        child = child->next_sibling;
    }
}

void Cmd_Copy(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: copy <src> <dst> [ALL] [CLONE] [DATES] [COM] [QUIET] [BUFFER <n>]");
        return;
    }

    int all   = cmd_kw_find(args, "ALL");
    int clone = cmd_kw_find(args, "CLONE");
    int com   = cmd_kw_find(args, "COM");
    int quiet = cmd_kw_find(args, "QUIET");
    /* DATES and BUFFER are parsed but have no effect in RamFS (no dates/buffer tuning) */

    /* Strip flags to leave src and dst */
    char clean[CMD_MAX_LINE];
    cmd_kw_strip(args, "ALL", NULL, clean, CMD_MAX_LINE);
    cmd_kw_strip(clean, "CLONE", NULL, clean, CMD_MAX_LINE);
    cmd_kw_strip(clean, "DATES", NULL, clean, CMD_MAX_LINE);
    cmd_kw_strip(clean, "COM", NULL, clean, CMD_MAX_LINE);
    cmd_kw_strip(clean, "QUIET", NULL, clean, CMD_MAX_LINE);
    /* BUFFER <n> */
    {
        const char *p = clean;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char *start = p;
            while (*p && *p != ' ') p++;
            int len = (int)(p - start);
            if (len == 6 &&
                ((start[0]=='B'||start[0]=='b') && (start[1]=='U'||start[1]=='u') &&
                 (start[2]=='F'||start[2]=='f') && (start[3]=='F'||start[3]=='f') &&
                 (start[4]=='E'||start[4]=='e') && (start[5]=='R'||start[5]=='r'))) {
                while (*p == ' ') p++;
                while (*p && *p != ' ') p++;
                break;
            }
        }
        /* Copy remainder back to clean */
        int ci = 0;
        while (*p && ci < CMD_MAX_LINE - 1) clean[ci++] = *p++;
        while (ci > 0 && clean[ci-1] == ' ') ci--;
        clean[ci] = '\0';
    }

    /* Split clean into src and dst at first space */
    char src[CMD_MAX_PATH], dst[CMD_MAX_PATH];
    const char *p = clean;
    int i = 0;
    while (*p && *p != ' ' && i < CMD_MAX_PATH - 1) { src[i++] = *p++; }
    src[i] = '\0';
    while (*p == ' ') p++;
    i = 0;
    while (*p && i < CMD_MAX_PATH - 1) { dst[i++] = *p++; }
    dst[i] = '\0';

    if (!src[0] || !dst[0]) {
        PRINT("Usage: copy <src> <dst> [ALL] [CLONE] [DATES] [COM] [QUIET] [BUFFER <n>]");
        return;
    }

    char abs_src[CMD_MAX_PATH], abs_dst[CMD_MAX_PATH];
    char src_pat[CMD_MAX_PATH];
    cmd_split_path_pat(ctx->cwd, src, abs_src, src_pat);
    cmd_make_abs(ctx->cwd, dst, abs_dst, CMD_MAX_PATH);

    if (src_pat[0]) {
        /* Pattern-based copy: source is a directory + pattern */
        VFS_MkDir(abs_dst);
        RamFsNode *dir = VFS_ResolveDir(abs_src);
        if (dir) {
            RamFsNode *child = dir->first_child;
            while (child) {
                if (cmd_pattern_match(child->name, src_pat)) {
                    char ssub[CMD_MAX_PATH], dsub[CMD_MAX_PATH];
                    cmd_scopy(ssub, abs_src, CMD_MAX_PATH);
                    cmd_scopy(dsub, abs_dst, CMD_MAX_PATH);
                    int sl = cmd_slen(ssub);
                    int dl = cmd_slen(dsub);
                    if (sl > 0 && ssub[sl - 1] != ':' && ssub[sl - 1] != '/') {
                        if (sl < CMD_MAX_PATH - 1) { ssub[sl] = '/'; ssub[sl + 1] = '\0'; }
                    }
                    if (dl > 0 && dsub[dl - 1] != ':' && dsub[dl - 1] != '/') {
                        if (dl < CMD_MAX_PATH - 1) { dsub[dl] = '/'; dsub[dl + 1] = '\0'; }
                    }
                    cmd_scat(ssub, child->name, CMD_MAX_PATH);
                    cmd_scat(dsub, child->name, CMD_MAX_PATH);
                    if (child->type == RAMFS_TYPE_DIR && all) {
                        copy_dir(ctx, ssub, dsub, clone, com, quiet, src_pat);
                    } else if (child->type != RAMFS_TYPE_DIR) {
                        copy_one(ctx, ssub, dsub, clone, com, quiet);
                    }
                }
                child = child->next_sibling;
            }
        }
    } else {
        /* Determine if source is a directory */
        RamFsNode *src_node = VFS_ResolveDir(abs_src);
        if (src_node && all) {
            copy_dir(ctx, abs_src, abs_dst, clone, com, quiet, NULL);
        } else {
            copy_one(ctx, abs_src, abs_dst, clone, com, quiet);
        }
    }
}
