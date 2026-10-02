/* cmd_netstat.c — C:netstat — live TCP/UDP socket table (UAOS-203)
 *
 *   netstat        TCP sockets: state, addrs, snd/rev depth, retrans, owner
 *                  UDP sockets: bound port, rx depth, last peer
 *                  usock layer: type, timeouts, TCP idx, owner
 */

#include "cmd_internal.h"

extern void Tcp_DiagDump(void *ctx, void (*emit)(void *, const char *));
extern void Udp_DiagDump(void *ctx, void (*emit)(void *, const char *));
extern void Usock_DiagDump(void *ctx, void (*emit)(void *, const char *));

static void ns_emit(void *ctx, const char *line)
{
    CMD_PRINT((NativeCmdCtx *)ctx, line);
}

void Cmd_Netstat(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    PRINT("== TCP ==");
    Tcp_DiagDump(ctx, ns_emit);
    PRINT("== UDP ==");
    Udp_DiagDump(ctx, ns_emit);
    PRINT("== usock ==");
    Usock_DiagDump(ctx, ns_emit);
}
