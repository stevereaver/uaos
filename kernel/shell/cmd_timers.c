/* cmd_timers.c — C:timers — timer.device pending-request dump (UAOS-201)
 *
 *   timers         one line per queued TimeRequest: fire tick, delta,
 *                  type, sigmask, signal mode
 */

#include "cmd_internal.h"

extern void TimerDevice_DiagDump(void *ctx, void (*emit)(void *, const char *));

static void tim_emit(void *ctx, const char *line)
{
    CMD_PRINT((NativeCmdCtx *)ctx, line);
}

void Cmd_Timers(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    TimerDevice_DiagDump(ctx, tim_emit);
}
