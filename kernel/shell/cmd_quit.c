/* cmd_quit.c — C:quit — exit from a script */

#include "cmd_internal.h"

void Cmd_Quit(NativeCmdCtx *ctx, const char *args)
{
    int rc = 0;
    if (args && *args) {
        const char *p = args;
        while (*p == ' ' || *p == '\t') p++;
        while (*p >= '0' && *p <= '9') {
            rc = rc * 10 + (*p - '0');
            p++;
        }
    }

    if (ctx->quit_script) {
        ctx->quit_script(ctx->shell_extra, rc);
    }

    char msg[CMD_MAX_LINE];
    cmd_scopy(msg, "QUIT ", CMD_MAX_LINE);
    char num[8];
    cmd_uint_to_dec((uint32_t)rc, num, 8);
    cmd_scat(msg, num, CMD_MAX_LINE);
    cmd_scat(msg, " (script termination requested)", CMD_MAX_LINE);
    PRINT(msg);
}
