/* cmd_jobs.c — C:jobs — list background jobs */

#include "cmd_internal.h"
#include "../display/shell_win.h"

void Cmd_Jobs(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    if (ctx->print && ctx->shell) {
        ShellWin_ListJobs(ctx->shell, ctx->print);
    } else {
        PRINT("Job listing not available.");
    }
}
