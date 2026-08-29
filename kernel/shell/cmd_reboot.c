/* cmd_reboot.c — C:reboot — warm reboot via keyboard controller */

#include "cmd_internal.h"
#include "../system_reboot.h"

void Cmd_Reboot(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    PRINT("Rebooting...");
    System_Reboot();
}
