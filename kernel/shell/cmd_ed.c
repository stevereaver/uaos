/* cmd_ed.c — C:ed — launch the ED editor */

#include "cmd_internal.h"
#include "../display/ed_win.h"

void Cmd_Ed(NativeCmdCtx *ctx, const char *args)
{
    char path[CMD_MAX_PATH];
    if (args && *args) {
        while (*args == ' ') args++;
        cmd_make_abs(ctx->cwd, args, path, CMD_MAX_PATH);
    } else {
        path[0] = '\0';
    }

    if (ctx->set_ed_mode) {
        ctx->set_ed_mode(ctx->shell_extra, path);
    } else {
        if (EdWin_Open(path) != 0) {
            CMD_PRINT(ctx, "ed: failed to open editor window");
        }
    }
}
