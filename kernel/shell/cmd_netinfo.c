/* cmd_netinfo.c — Tools:NetInfo — launch the NetInfo window */

#include "cmd_internal.h"
#include "../display/netinfo_win.h"

void Cmd_NetInfo(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    (void)ctx;
    NetInfoWin_Open();
}
