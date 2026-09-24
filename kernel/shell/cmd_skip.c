/* cmd_skip.c — C:skip — skip to a label in the running script
 *
 * Inside a script the interpreter intercepts SKIP lines before command
 * dispatch (see script_exec_line in shell_win.c), so this native entry is
 * only reached when SKIP is invoked outside the script engine — where
 * there is no script context to act on.  It exists so that C:skip
 * resolves like the other C: commands.
 */

#include "cmd_internal.h"

void Cmd_Skip(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    PRINT("Skip is only meaningful inside a script.");
}
