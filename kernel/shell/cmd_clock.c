/* cmd_clock.c — Tools:Clock — launch the Clock window */
#include "cmd_internal.h"
#include "../display/clock_win.h"

void Cmd_ClockWin(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    (void)ctx;
    ClockWin_Open();
}
