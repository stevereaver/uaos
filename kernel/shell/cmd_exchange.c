/* cmd_exchange.c — Exchange command
 *
 * Opens the Commodities Exchange window.
 */

#include "native_cmd.h"

void Cmd_Exchange(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    extern void ExchangeWin_Show(void);
    ExchangeWin_Show();
    if (ctx->print) ctx->print(ctx->shell, "Exchange opened.");
}
