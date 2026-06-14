/* cmd_stack.c — C:stack — show/set default stack size */

#include "cmd_internal.h"

void Cmd_Stack(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    PRINT("Default stack size: 16 KB (bootstrap)");
    PRINT("Command stack size: 4 KB per shell");
    if (args && *args) {
        PRINT("Note: dynamic stack resizing not yet implemented.");
    }
}
