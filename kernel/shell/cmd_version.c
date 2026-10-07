/* cmd_version.c — C:version — display OS version information */

#include "cmd_internal.h"
#include "../display/framebuffer.h"
#include "../dbg/diag.h"

void Cmd_Version(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    PRINT("Ultimate Amiga OS  v0.1.0-dev");
    PRINT("Kernel: x86_64 ELF64, Multiboot2, long mode");

    char res[96];
    char num[12];
    cmd_scopy(res, "Display: ", 96);
    cmd_uint_to_dec(g_fb.width,  num, 12); cmd_scat(res, num, 96);
    cmd_scat(res, "x", 96);
    cmd_uint_to_dec(g_fb.height, num, 12); cmd_scat(res, num, 96);
    cmd_scat(res, " ", 96);
    cmd_uint_to_dec(g_fb.bpp,    num, 12); cmd_scat(res, num, 96);
    cmd_scat(res, "bpp linear framebuffer", 96);
    PRINT(res);

    char inp[64];
    SysInfo_InputDesc(inp, sizeof(inp));
    cmd_scopy(res, "Input: ", 96);
    cmd_scat(res, inp, 96);
    PRINT(res);
}
