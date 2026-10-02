/* cmd_sercon.c — C:sercon — serial console control (UAOS-207)
 *
 *   sercon          status
 *   sercon ON       start/enable polled UART command task
 *   sercon OFF      stop accepting commands (task idles)
 */

#include "cmd_internal.h"
#include "../klog/klog.h"

void Cmd_Sercon(NativeCmdCtx *ctx, const char *args)
{
    if (args && cmd_kw_find(args, "on")) {
        Sercon_Start();
        PRINT("sercon: enabled — serial console live on COM1");
        return;
    }
    if (args && cmd_kw_find(args, "off")) {
        Sercon_Stop();
        PRINT("sercon: disabled");
        return;
    }
    PRINT(Sercon_Running()
          ? "sercon: running (type 'help' on the serial line)"
          : "sercon: off — 'sercon on' to enable, or boot arg 'sercon'");
}
