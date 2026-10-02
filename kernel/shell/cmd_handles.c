/* cmd_handles.c — C:handles — open-file/lock handle dump (UAOS-202)
 *
 *   handles        one line per live HandleEntry: type, path, owning
 *                  task name, flags/lock access
 */

#include "cmd_internal.h"

extern void HandleTable_DiagDump(void *ctx, void (*emit)(void *, const char *));

static void handles_emit(void *ctx, const char *line)
{
    CMD_PRINT((NativeCmdCtx *)ctx, line);
}

void Cmd_Handles(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    HandleTable_DiagDump(ctx, handles_emit);
}
