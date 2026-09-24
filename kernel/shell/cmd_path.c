/* cmd_path.c — C:path — show or set the command search path
 *
 * The search path lives in the ShellInstance, so the real work is done by
 * the shell built-in.  This native entry exists so that C:path resolves
 * like the other C: commands; it forwards to the built-in handler.
 */

#include "cmd_internal.h"

void Cmd_Path(NativeCmdCtx *ctx, const char *args)
{
    if (!ctx->dispatch_line || !ctx->shell_extra) {
        PRINT("path: shell dispatch not available");
        return;
    }

    char line[CMD_MAX_LINE];
    cmd_scopy(line, "path", CMD_MAX_LINE);
    if (args && *args) {
        cmd_scat(line, " ", CMD_MAX_LINE);
        cmd_scat(line, args, CMD_MAX_LINE);
    }
    ctx->dispatch_line(ctx->shell_extra, line);
}
