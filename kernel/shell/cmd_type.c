/* cmd_type.c — C:type — print file contents to the shell */

#include "cmd_internal.h"

void Cmd_Type(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) { PRINT("Usage: type <file>"); return; }
    char path[CMD_MAX_PATH];
    cmd_make_abs(ctx->cwd, args, path, CMD_MAX_PATH);

    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ)) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "Cannot open: ", CMD_MAX_LINE);
        cmd_scat(msg, path, CMD_MAX_LINE);
        PRINT(msg);
        return;
    }

    uint8_t buf[CMD_MAX_LINE];
    uint32_t pos  = 0;
    uint32_t size = VFS_Size(&fh);
    while (pos < size) {
        int col = 0;
        while (pos < size && col < CMD_MAX_LINE - 1) {
            uint8_t c;
            if (VFS_Read(&fh, &c, 1) == 0) break;
            pos++;
            if (c == '\n') break;
            if (c != '\r') buf[col++] = c;
        }
        buf[col] = '\0';
        PRINT((char *)buf);
    }
    VFS_Close(&fh);
}
