/* cmd_vim.c — C:vim — launch the Vim editor */

#include "cmd_internal.h"
#include "../display/vim_win.h"

void Cmd_Vim(NativeCmdCtx *ctx, const char *args)
{
    char path[CMD_MAX_PATH];
    if (args && *args) {
        while (*args == ' ') args++;
        cmd_make_abs(ctx->cwd, args, path, CMD_MAX_PATH);
    } else {
        path[0] = '\0';
    }
    if (ctx->set_vim_mode) {
        ctx->set_vim_mode(ctx->shell_extra, path);
    } else {
        /* Fallback to standalone window when not called from a shell */
        if (VimWin_Open(path) != 0) {
            CMD_PRINT(ctx, "vim: failed to open editor window");
        }
    }
}
