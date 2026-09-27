/* cmd_clear.c — C:clear — clear the shell window
 *
 * Clearing the shell history buffer needs ShellInstance internals, so
 * the real work lives in the shell's clear_history callback (which also
 * emits ESC[2J ESC[H for remote/telnet sessions). */

#include "cmd_internal.h"

void Cmd_Clear(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    if (ctx->clear_history)
        ctx->clear_history(ctx->shell_extra);
}
