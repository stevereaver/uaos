/* cmd_ports.c — C:ports — handler MsgPort/packet queue dump (UAOS-200)
 *
 *   ports          one line per handler + async pending packet list
 *
 * UAOS DOS handlers hold a synchronous MsgPort (dospacket.h); pending
 * packets on mp_MsgList plus the SendPktAsync queue are the stuck/leaked
 * state strace's call log can't show.
 */

#include "cmd_internal.h"
#include "../dos/handler.h"

static void ports_emit(void *ctx, const char *line)
{
    CMD_PRINT((NativeCmdCtx *)ctx, line);
}

void Cmd_Ports(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    Handler_DiagDump(ctx, ports_emit);
}
