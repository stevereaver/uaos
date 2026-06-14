/* cmd_getenv.c — C:getenv — read environment variable */

#include "cmd_internal.h"

void Cmd_GetEnv(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: getenv <name>");
        return;
    }

    const char *p = args;
    while (*p == ' ' || *p == '\t') p++;

    char name[32];
    int i = 0;
    while (*p && *p != ' ' && *p != '\t' && i < 31)
        name[i++] = *p++;
    name[i] = '\0';

    if (!name[0]) {
        PRINT("Usage: getenv <name>");
        return;
    }

    char value[128];
    if (ctx->get_env && ctx->get_env(ctx->shell_extra, name, value, sizeof(value))) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, name, CMD_MAX_LINE);
        cmd_scat(msg, "=", CMD_MAX_LINE);
        cmd_scat(msg, value, CMD_MAX_LINE);
        PRINT(msg);
    } else {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "Variable not found: ", CMD_MAX_LINE);
        cmd_scat(msg, name, CMD_MAX_LINE);
        PRINT(msg);
    }
}
