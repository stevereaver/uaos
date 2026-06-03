/* cmd_delete.c — C:delete — delete a file or empty directory */

#include "cmd_internal.h"

void Cmd_Delete(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) { PRINT("Usage: delete <path>"); return; }
    char path[CMD_MAX_PATH];
    cmd_make_abs(ctx->cwd, args, path, CMD_MAX_PATH);
    int rc = VFS_Delete(path);
    if (rc == 0) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "Deleted: ", CMD_MAX_LINE);
        cmd_scat(msg, path, CMD_MAX_LINE);
        PRINT(msg);
    } else if (rc == -2) {
        PRINT("Directory not empty.");
    } else {
        PRINT("Not found.");
    }
}
