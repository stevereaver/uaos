/* cmd_makedir.c — C:makedir — create a directory */

#include "cmd_internal.h"

void Cmd_Makedir(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) { PRINT("Usage: makedir <path>"); return; }
    char path[CMD_MAX_PATH];
    cmd_make_abs(ctx->cwd, args, path, CMD_MAX_PATH);
    int rc = VFS_MkDir(path);
    if (rc == 0) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "Created: ", CMD_MAX_LINE);
        cmd_scat(msg, path, CMD_MAX_LINE);
        PRINT(msg);
    } else {
        PRINT("Failed to create directory.");
    }
}
