/* cmd_run.c — C:run — execute an embedded Amiga M68k binary */

#include "cmd_internal.h"
#include "../../emulation/uaos_emu.h"

void Cmd_Run(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: run <program> [args]");
        PRINT("Runs an embedded Amiga binary from the ROM registry.");
        return;
    }

    UAOS_Emu_SetCwd(ctx->cwd);
    UAOS_Emu_RunByName(args, ctx->shell, (UAOS_PrintFn)ctx->print);
}
