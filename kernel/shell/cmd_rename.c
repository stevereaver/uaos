/* cmd_rename.c — C:rename — rename or move a file */

#include "cmd_internal.h"

void Cmd_Rename(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) { PRINT("Usage: rename <old> <new>"); return; }

    /* Split args into src and dst at first space */
    char src[CMD_MAX_PATH], dst[CMD_MAX_PATH];
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < CMD_MAX_PATH - 1) { src[i++] = *p++; }
    src[i] = '\0';
    while (*p == ' ') p++;
    i = 0;
    while (*p && i < CMD_MAX_PATH - 1) { dst[i++] = *p++; }
    dst[i] = '\0';

    if (!src[0] || !dst[0]) { PRINT("Usage: rename <old> <new>"); return; }

    char abs_src[CMD_MAX_PATH], abs_dst[CMD_MAX_PATH];
    cmd_make_abs(ctx->cwd, src, abs_src, CMD_MAX_PATH);
    cmd_make_abs(ctx->cwd, dst, abs_dst, CMD_MAX_PATH);

    if (VFS_Rename(abs_src, abs_dst) == 0) {
        PRINT("Renamed successfully.");
    } else {
        PRINT("Rename failed.");
    }
}
