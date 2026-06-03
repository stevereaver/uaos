/* cmd_date.c — C:date — display current date/time */

#include "cmd_internal.h"

void Cmd_Date(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    /* UAOS doesn't have a real-time clock yet, display build date */
    PRINT("Ultimate Amiga OS - Build Date: 2026");
    PRINT("Note: Real-time clock not yet implemented");
}
