/* cmd_mem.c — C:mem — display memory information */

#include "cmd_internal.h"

void Cmd_Mem(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    PRINT("RAM:  512 MB (QEMU)");
    PRINT("Kernel load: 0x0000000000100000");
    PRINT("Framebuffer: mapped (GOP physical address)");
    PRINT("Stack: 16 KB (bootstrap), no heap allocator yet");
}
