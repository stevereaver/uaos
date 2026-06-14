/* cmd_why.c — C:why — show last error code explanation */

#include "cmd_internal.h"

void Cmd_Why(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    int rc = 0;
    if (ctx->get_last_rc) {
        rc = ctx->get_last_rc(ctx->shell_extra);
    }

    char msg[CMD_MAX_LINE];
    cmd_scopy(msg, "Last return code: ", CMD_MAX_LINE);
    char num[8];
    cmd_uint_to_dec((uint32_t)(rc < 0 ? -rc : rc), num, 8);
    cmd_scat(msg, num, CMD_MAX_LINE);
    PRINT(msg);

    switch (rc) {
        case 0:  PRINT("(Success)"); break;
        case 5:  PRINT("(Device not mounted)"); break;
        case 20: PRINT("(Object not found)"); break;
        case 21: PRINT("(Invalid lock)"); break;
        case 22: PRINT("(Object wrong type)"); break;
        case 26: PRINT("(Disk full)"); break;
        case 28: PRINT("(Write error)"); break;
        case 29: PRINT("(Read error)"); break;
        case 32: PRINT("(Not a DOS disk)"); break;
        case 33: PRINT("(No disk)"); break;
        default:
            if (rc != 0) {
                PRINT("(Unknown error)");
            }
            break;
    }
}
