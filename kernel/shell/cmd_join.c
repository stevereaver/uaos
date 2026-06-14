/* cmd_join.c — C:join — concatenate multiple files */

#include "cmd_internal.h"

void Cmd_Join(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: join <file1> [file2] ...");
        PRINT("Concatenates files and prints to stdout.");
        return;
    }

    /* Parse space-separated file list */
    const char *p = args;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        char name[CMD_MAX_PATH];
        int i = 0;
        while (*p && *p != ' ' && *p != '\t' && i < CMD_MAX_PATH - 1)
            name[i++] = *p++;
        name[i] = '\0';

        char path[CMD_MAX_PATH];
        cmd_make_abs(ctx->cwd, name, path, CMD_MAX_PATH);

        VfsFile fh;
        if (!VFS_Open(&fh, path, VFS_READ)) {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "Cannot open: ", CMD_MAX_LINE);
            cmd_scat(msg, path, CMD_MAX_LINE);
            PRINT(msg);
            continue;
        }

        uint32_t pos = 0;
        uint32_t sz = VFS_Size(&fh);
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
            PRINT((char *)buf);
        }
        VFS_Close(&fh);
    }
}
