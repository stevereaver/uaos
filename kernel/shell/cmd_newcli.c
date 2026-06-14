/* cmd_newcli.c — C:newcli — open a new shell window
 *
 * Syntax: newcli [from <script>]
 * Alias: newshell (same functionality)
 */

#include "cmd_internal.h"
#include "../display/shell_win.h"

void Cmd_NewCLI(NativeCmdCtx *ctx, const char *args)
{
    (void)args;  /* "from" script support can be added later */

    /* Check if we've hit the shell limit */
    /* Note: ShellWin_Open() internally handles MAX_SHELLS check */
    ShellWin_Open();

    PRINT("New shell window opened.");
}
