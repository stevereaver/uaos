/* cmd_ps.c — C:ps — list running tasks
 *
 * Displays all active tasks in the system:
 *   - Shell CLI windows
 *   - Vim editors (inline and standalone)
 *   - Other WM windows
 *
 * Usage: ps
 */

#include "cmd_internal.h"

void Cmd_Ps(NativeCmdCtx *ctx, const char *args)
{
    (void)args;

    if (!ctx->enum_tasks) {
        PRINT("Task enumeration not available.");
        return;
    }

    PRINT("Task                 Type     Info");
    PRINT("----------------------------------------");

    int idx = 0;
    char line[96];
    while (ctx->enum_tasks(ctx->shell_extra, idx, line, sizeof(line))) {
        PRINT(line);
        idx++;
    }

    if (idx == 0) {
        PRINT("(no active tasks)");
    }
}
