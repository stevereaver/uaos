/* cmd_unset.c — C:unset — remove local environment variable */

#include "cmd_internal.h"

void Cmd_UnSet(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: unset <name>");
        return;
    }

    /* Dispatch to the shell's builtin unset command */
    char cmd[CMD_MAX_LINE];
    cmd_scopy(cmd, "unset ", CMD_MAX_LINE);
    cmd_scat(cmd, args, CMD_MAX_LINE);

    if (ctx->dispatch_line) {
        ctx->dispatch_line(ctx->shell_extra, cmd);
    } else {
        PRINT("Error: dispatch_line not available");
    }
}
