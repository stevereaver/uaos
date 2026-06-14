/* cmd_wait.c — C:wait — pause for N seconds or until a specific time */

#include "cmd_internal.h"

void Cmd_Wait(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: wait <seconds>");
        PRINT("Pauses script execution for the specified number of seconds.");
        return;
    }

    /* Parse seconds */
    int seconds = 0;
    const char *p = args;
    while (*p == ' ' || *p == '\t') p++;
    while (*p >= '0' && *p <= '9') {
        seconds = seconds * 10 + (*p - '0');
        p++;
    }

    if (seconds <= 0) {
        PRINT("Wait: invalid duration");
        return;
    }

    char msg[CMD_MAX_LINE];
    cmd_scopy(msg, "Waiting ", CMD_MAX_LINE);
    char sn[8];
    cmd_uint_to_dec((uint32_t)seconds, sn, 8);
    cmd_scat(msg, sn, CMD_MAX_LINE);
    cmd_scat(msg, " second(s)...", CMD_MAX_LINE);
    PRINT(msg);

    for (int i = 0; i < seconds; i++) {
        CMD_YIELD(ctx, 1000);
    }
}
