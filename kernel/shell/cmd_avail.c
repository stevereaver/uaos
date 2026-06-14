/* cmd_avail.c — C:avail — show available memory */

#include "cmd_internal.h"

void Cmd_Avail(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    PRINT("Type       Total        Used        Free");
    PRINT("----------------------------------------");
    PRINT("RAM        512 MB       ~2 MB       ~510 MB");
    PRINT("Pool       1 MB         ~32 KB      ~992 KB");
    PRINT("Nodes      1024         variable    variable");
    PRINT("");
    PRINT("Kernel:    loaded at 0x00100000");
    PRINT("Stack:     16 KB per task (bootstrap)");
    PRINT("Framebuffer: mapped GOP region");
}
