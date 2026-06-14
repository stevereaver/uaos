/* cmd_endcli.c — C:endcli — close the current CLI */

#include "cmd_internal.h"

void Cmd_EndCLI(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    PRINT("Closing CLI...");
    if (ctx->close_shell) {
        ctx->close_shell(ctx->shell_extra);
    }
}
