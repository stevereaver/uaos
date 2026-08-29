/* cmd_guide.c — C:guide — launch the AmigaGuide help viewer */

#include "cmd_internal.h"

void Cmd_Guide(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    /* Launch Tools:Guide — the userspace AmigaGuide viewer */
    if (ctx->dispatch_line) {
        ctx->dispatch_line(ctx->shell_extra, "SYS:Tools/Guide");
    } else {
        CMD_PRINT(ctx, "guide: requires shell context to launch viewer");
    }
}
