/* cmd_version.c — C:version — display OS version information */

#include "cmd_internal.h"
#include "../display/framebuffer.h"

void Cmd_Version(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    PRINT("Ultimate Amiga OS  v0.1.0-dev");
    PRINT("Kernel: x86_64 ELF64, Multiboot2, long mode");

    char res[48];
    char num[12];
    cmd_scopy(res, "Display: ", 48);
    cmd_uint_to_dec(g_fb.width,  num, 12); cmd_scat(res, num, 48);
    cmd_scat(res, "x", 48);
    cmd_uint_to_dec(g_fb.height, num, 12); cmd_scat(res, num, 48);
    cmd_scat(res, " ", 48);
    cmd_uint_to_dec(g_fb.bpp,    num, 12); cmd_scat(res, num, 48);
    cmd_scat(res, "bpp linear framebuffer", 48);
    PRINT(res);

    PRINT("Input: PS/2 keyboard + mouse, IRQ1/IRQ12");
}
