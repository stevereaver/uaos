/* cmd_delete.c — C:delete — delete a file or directory */

#include "cmd_internal.h"

void Cmd_Delete(NativeCmdCtx *ctx, const char *args)
{
    (void)args;

    if (!ctx->template) {
        PRINT("Usage: delete <path> [ALL] [QUIET] [FORCE]");
        return;
    }

    int all   = CmdTemplate_GetSwitch(ctx->template, "ALL");
    int quiet = CmdTemplate_GetSwitch(ctx->template, "QUIET");
    int force = CmdTemplate_GetSwitch(ctx->template, "FORCE");

    const char *file = CmdTemplate_GetString(ctx->template, "FILE");
    if (!file) {
        PRINT("Usage: delete <path> [ALL] [QUIET] [FORCE]");
        return;
    }

    char path[CMD_MAX_PATH];
    char pat[CMD_MAX_PATH];
    cmd_split_path_pat(ctx->cwd, file, path, pat);

    int rc;
    if (all || pat[0]) {
        rc = cmd_delete_recursive(path, force, pat);
    } else {
        rc = VFS_Delete(path);
        if (rc == -4 && force) {
            uint16_t p = VFS_GetProtection(path);
            VFS_SetProtection(path, p & ~FIBF_DELETE);
            rc = VFS_Delete(path);
        }
    }

    if (!quiet) {
        if (rc == 0) {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "Deleted: ", CMD_MAX_LINE);
            cmd_scat(msg, path, CMD_MAX_LINE);
            if (pat[0]) {
                cmd_scat(msg, " (pattern: ", CMD_MAX_LINE);
                cmd_scat(msg, pat, CMD_MAX_LINE);
                cmd_scat(msg, ")", CMD_MAX_LINE);
            }
            PRINT(msg);
        } else if (rc == -2) {
            PRINT("Directory not empty.");
        } else if (rc == -1) {
            PRINT("Not found.");
        } else {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "Failed to delete: ", CMD_MAX_LINE);
            cmd_scat(msg, path, CMD_MAX_LINE);
            PRINT(msg);
        }
    }
}
