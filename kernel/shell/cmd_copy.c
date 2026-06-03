/* cmd_copy.c — C:copy — copy a file */

#include "cmd_internal.h"

void Cmd_Copy(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) { PRINT("Usage: copy <src> <dst>"); return; }

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

    if (!src[0] || !dst[0]) { PRINT("Usage: copy <src> <dst>"); return; }

    char abs_src[CMD_MAX_PATH], abs_dst[CMD_MAX_PATH];
    cmd_make_abs(ctx->cwd, src, abs_src, CMD_MAX_PATH);
    cmd_make_abs(ctx->cwd, dst, abs_dst, CMD_MAX_PATH);

    VfsFile fsrc;
    if (!VFS_Open(&fsrc, abs_src, VFS_READ)) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "Cannot open source: ", CMD_MAX_LINE);
        cmd_scat(msg, abs_src, CMD_MAX_LINE);
        PRINT(msg);
        return;
    }

    VfsFile fdst;
    if (!VFS_Open(&fdst, abs_dst, VFS_WRITE)) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "Cannot open destination: ", CMD_MAX_LINE);
        cmd_scat(msg, abs_dst, CMD_MAX_LINE);
        PRINT(msg);
        VFS_Close(&fsrc);
        return;
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

    char msg[CMD_MAX_LINE];
    cmd_scopy(msg, "Copied ", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)total, msg + cmd_slen(msg), 12);
    cmd_scat(msg, " bytes", CMD_MAX_LINE);
    PRINT(msg);
}
