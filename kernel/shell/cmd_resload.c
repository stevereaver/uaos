/* cmd_resload.c — C:resload — load a command into the resident list
 *
 * Shorthand for "resident <cmd>": resolves the command through C:/PATH
 * and caches it in memory so later invocations skip disk access.
 * Any resident subcommand is accepted (pure, remove, flush); with no
 * arguments it lists the resident list, same as "resident".
 */

#include "cmd_internal.h"

void Cmd_ResLoad(NativeCmdCtx *ctx, const char *args)
{
    if (!ctx->dispatch_line || !ctx->shell_extra) {
        PRINT("resload: shell dispatch not available");
        return;
    }

    char line[CMD_MAX_LINE];
    cmd_scopy(line, "resident", CMD_MAX_LINE);
    if (args && *args) {
        cmd_scat(line, " ", CMD_MAX_LINE);
        cmd_scat(line, args, CMD_MAX_LINE);
    }
    ctx->dispatch_line(ctx->shell_extra, line);
}
