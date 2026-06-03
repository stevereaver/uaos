/* cmd_reboot.c — C:reboot — warm reboot via keyboard controller */

#include "cmd_internal.h"

void Cmd_Reboot(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    PRINT("Rebooting...");
    __asm__ volatile (
        "1: inb  $0x64, %%al\n"
        "   testb $0x02, %%al\n"
        "   jnz 1b\n"
        "   movb $0xFE, %%al\n"
        "   outb %%al, $0x64\n"
        :: : "eax"
    );
    for (;;) __asm__ volatile ("hlt");
}
