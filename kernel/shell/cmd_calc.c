/* cmd_calc.c — C:calculator — launch the Calculator window */

#include "cmd_internal.h"
#include "../display/calc_win.h"

void Cmd_CalcWin(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    (void)ctx;
    CalcWin_Open();
}
