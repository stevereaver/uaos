/* cmd_failat.c — C:failat — set or show the failure threshold */

#include "cmd_internal.h"

void Cmd_Failat(NativeCmdCtx *ctx, const char *args)
{
    int threshold = 10; /* AmigaDOS default */

    if (args && *args) {
        const char *p = args;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9') {
            threshold = 0;
            while (*p >= '0' && *p <= '9') {
                threshold = threshold * 10 + (*p - '0');
                p++;
            }
        }
    }

    if (ctx->set_failat) {
        ctx->set_failat(ctx->shell_extra, threshold);
    }

    char msg[CMD_MAX_LINE];
    cmd_scopy(msg, "FAILAT threshold: ", CMD_MAX_LINE);
    char num[8];
    cmd_uint_to_dec((uint32_t)threshold, num, 8);
    cmd_scat(msg, num, CMD_MAX_LINE);
    PRINT(msg);
}
