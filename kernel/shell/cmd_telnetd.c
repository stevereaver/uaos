/* cmd_telnetd.c — C:telnetd — start/stop the remote shell service */

#include "cmd_internal.h"
#include "../net/telnetd.h"
#include "../net/stack.h"

static void set_rc(NativeCmdCtx *ctx, int rc)
{
    if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, rc);
}

void Cmd_Telnetd(NativeCmdCtx *ctx, const char *args)
{
    (void)args;  /* parsed via ctx->template ("PORT/K/N,STOP/S") */

    /* STOP is handled first so it still works while the stack is down. */
    if (ctx->template &&
        CmdTemplate_GetSwitch(ctx->template, "STOP")) {
        if (!Telnetd_IsRunning()) {
            PRINT("telnetd: not running");
            set_rc(ctx, 10);
            return;
        }
        /* Print before stopping: on a remote session the daemon teardown
         * kills our own socket, so a message printed after would never
         * reach the client. */
        PRINT("telnetd: stopping");
        Telnetd_Stop();
        return;
    }

    int port = TELNETD_DEFAULT_PORT;
    if (ctx->template &&
        CmdTemplate_GetString(ctx->template, "PORT")) {
        int v;
        if (!CmdTemplate_GetInt(ctx->template, "PORT", &v) ||
            v <= 0 || v > 65535) {
            PRINT("telnetd: bad arguments — usage: telnetd [PORT=n] [STOP]");
            set_rc(ctx, 20);
            return;
        }
        port = v;
    }

    if (!net_stack_is_up()) {
        PRINT("telnetd: network stack is down (run net-start first)");
        set_rc(ctx, 20);
        return;
    }
    if (Telnetd_IsRunning()) {
        PRINT("telnetd: already running");
        set_rc(ctx, 10);
        return;
    }
    if (!Telnetd_Start((uint16_t)port)) {
        PRINT("telnetd: failed to start (listener unavailable)");
        set_rc(ctx, 20);
        return;
    }
    char msg[64];
    cmd_scopy(msg, "telnetd: listening on port ", sizeof(msg));
    char num[8];
    cmd_uint_to_dec((uint32_t)port, num, sizeof(num));
    cmd_scat(msg, num, sizeof(msg));
    cmd_scat(msg, " - no login required", sizeof(msg));
    PRINT(msg);
}
