/* cmd_clear.c — C:clear — clear the shell window
 *
 * Note: clearing the shell history buffer requires access to the
 * ShellInstance internals.  The shell itself intercepts "clear" before
 * dispatching to the binary layer so that it can reset its own state;
 * this file provides the NativeCmdCtx-compatible wrapper for completeness
 * and for any future shell-agnostic clear mechanism.
 */

#include "cmd_internal.h"

/* ShellInstance clear is handled specially by the shell dispatcher so that
 * it can reset the internal history ring-buffer.  If this binary stub is
 * reached directly (e.g. via explicit C:clear), emit a notice. */
void Cmd_Clear(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    /* The shell pre-intercepts 'clear' to wipe its own buffer; if we ever
     * reach here it means the shell did not intercept — just print nothing
     * so the effect is visually similar. */
    (void)ctx;
}
