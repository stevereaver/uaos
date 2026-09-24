/* cmd_lab.c — C:lab — script label marker
 *
 * The script engine pre-scans LAB lines as jump targets for SKIP and
 * treats them as no-ops at run time (see run_script_text in shell_win.c).
 * As a standalone command LAB is likewise a no-op — it exists so that
 * C:lab resolves like the other C: commands.
 */

#include "cmd_internal.h"

void Cmd_Lab(NativeCmdCtx *ctx, const char *args)
{
    (void)ctx;
    (void)args;
}
