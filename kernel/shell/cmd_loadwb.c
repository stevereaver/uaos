/* cmd_loadwb.c — C:loadwb — launch the Workbench desktop */

#include "cmd_internal.h"
#include "../display/desktop.h"
#include "../display/wm.h"

void Cmd_LoadWB(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    PRINT("LoadWB — launching Workbench desktop...");
    if (ctx->loadwb) {
        ctx->loadwb();
    } else {
        Desktop_MarkWorkbenchLoaded();
        Desktop_Draw();
        WM_Redraw();
    }
}
