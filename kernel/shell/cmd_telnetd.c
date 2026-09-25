/* cmd_telnetd.c — C:telnetd — start the remote shell service */

#include "cmd_internal.h"
#include "../net/telnetd.h"
#include "../net/stack.h"

static int ci_prefix(const char *s, const char *kw)
{
    while (*kw) {
        char a = *s, b = *kw;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
        s++; kw++;
    }
    return 1;
}

static int parse_port(const char *args, uint16_t *out)
{
    *out = TELNETD_DEFAULT_PORT;
    if (!args || !*args) return 1;

    /* Accept: PORT=2323 / PORT 2323 / 2323 (case-insensitive keyword) */
    const char *p = args;
    while (*p == ' ' || *p == '\t') p++;
    if (ci_prefix(p, "PORT")) {
        p += 4;
        while (*p == ' ' || *p == '\t' || *p == '=') p++;
    }
    if (*p < '0' || *p > '9') return 0;

    unsigned v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (unsigned)(*p - '0');
        if (v > 65535) return 0;
        p++;
    }
    if (v == 0) return 0;
    *out = (uint16_t)v;
    return 1;
}

static void set_rc(NativeCmdCtx *ctx, int rc)
{
    if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, rc);
}

void Cmd_Telnetd(NativeCmdCtx *ctx, const char *args)
{
    uint16_t port;
    if (!parse_port(args, &port)) {
        PRINT("telnetd: bad arguments — usage: telnetd [PORT=n]");
        set_rc(ctx, 20);
        return;
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
    if (!Telnetd_Start(port)) {
        PRINT("telnetd: failed to start (listener unavailable)");
        set_rc(ctx, 20);
        return;
    }
    char msg[48];
    cmd_scopy(msg, "telnetd: listening on port ", sizeof(msg));
    char num[8];
    cmd_uint_to_dec(port, num, sizeof(num));
    cmd_scat(msg, num, sizeof(msg));
    cmd_scat(msg, " — no login required", sizeof(msg));
    PRINT(msg);
}
