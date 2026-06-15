/* cmd_echo.c — C:echo — print text to the shell */

#include "cmd_internal.h"

void Cmd_Echo(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    const char *text = "";
    int noline = 0;

    if (ctx->template) {
        const char *s = CmdTemplate_GetString(ctx->template, "STRING");
        if (s) text = s;
        noline = CmdTemplate_GetSwitch(ctx->template, "NOLINE");
    }

    if (noline && ctx->print_raw) {
        ctx->print_raw(ctx->shell, text);
    } else if (text[0]) {
        PRINT(text);
    } else if (!noline) {
        PRINT("");
    }
}
