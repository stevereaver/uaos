/* cmd_prompt.c — C:prompt — set the shell prompt string */

#include "cmd_internal.h"

void Cmd_Prompt(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        /* Reset to default */
        if (ctx->set_prompt) {
            ctx->set_prompt(ctx->shell_extra, "");
        }
        PRINT("Prompt reset to default.");
        return;
    }

    if (ctx->set_prompt) {
        ctx->set_prompt(ctx->shell_extra, args);
    }

    char msg[CMD_MAX_LINE];
    cmd_scopy(msg, "Prompt set to: ", CMD_MAX_LINE);
    cmd_scat(msg, args, CMD_MAX_LINE);
    PRINT(msg);
}
