/* cmd_pointer.c — C:pointer — open pointer preferences panel */

#include "cmd_internal.h"
#include "../display/pointer_prefs.h"

void Cmd_Pointer(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    (void)ctx;
    PointerPrefs_Show();
}
