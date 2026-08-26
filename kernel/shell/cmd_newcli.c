/* cmd_newcli.c — C:newcli — open a new shell window
 *
 * Syntax: newcli [from <script>]
 * Alias: newshell (same functionality)
 *
 * When "from <script>" is supplied, the script is resolved relative to
 * the invoking shell's cwd and executed in the newly opened shell window
 * (mirroring the S:Startup-Sequence mechanism used at boot).
 */

#include "cmd_internal.h"
#include "../display/shell_win.h"

void Cmd_NewCLI(NativeCmdCtx *ctx, const char *args)
{
    char script_path[CMD_MAX_PATH];
    script_path[0] = '\0';

    /* Parse the optional "from <script>" keyword (case-insensitive). */
    if (args && *args) {
        char cleaned[CMD_MAX_LINE];
        if (cmd_kw_strip(args, "from", NULL, cleaned, CMD_MAX_LINE)) {
            /* "from" keyword was present — the remaining text is the path */
            const char *p = cleaned;
            while (*p == ' ' || *p == '\t') p++;
            int i = 0;
            while (*p && *p != ' ' && *p != '\t' && i < CMD_MAX_PATH - 1) {
                script_path[i++] = *p++;
            }
            script_path[i] = '\0';

            if (!script_path[0]) {
                PRINT("Usage: newcli [from <script>]");
                return;
            }
        } else if (*cleaned) {
            /* Non-empty argument without "from" — reject to avoid
             * silently ignoring user input. */
            PRINT("Usage: newcli [from <script>]");
            return;
        }
    }

    /* Resolve the script path relative to the invoking shell's cwd so
     * the new shell (which defaults to RAM:) sees the right file. */
    char full_path[CMD_MAX_PATH];
    if (script_path[0]) {
        cmd_make_abs(ctx->cwd, script_path, full_path, CMD_MAX_PATH);
        ShellWin_OpenWithScript(full_path);
    } else {
        /* Note: ShellWin_Open() internally handles MAX_SHELLS check */
        ShellWin_Open();
    }

    PRINT("New shell window opened.");
}
