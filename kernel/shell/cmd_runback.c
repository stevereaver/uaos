/* cmd_runback.c — C:runback — run a command in the background
 *
 * Equivalent to appending '&' to a command line: the line is queued as a
 * background job and executed by the shell's job pump, so the prompt
 * returns immediately.  Named for the classic Amiga RunBack utility that
 * started CLI programs without tying up the shell window.
 */

#include "cmd_internal.h"

void Cmd_RunBack(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: runback <command> [args]");
        PRINT("Runs the command as a background job (same as '<command> &').");
        return;
    }

    if (!ctx->dispatch_line || !ctx->shell_extra) {
        PRINT("runback: shell dispatch not available");
        return;
    }

    /* Build "<args> &" — inst_dispatch detects the trailing '&' and
     * enqueues the line as a background job.  Leave room for " &". */
    char line[CMD_MAX_LINE];
    cmd_scopy(line, args, CMD_MAX_LINE - 2);
    int len = cmd_slen(line);
    while (len > 0 && line[len - 1] == ' ') line[--len] = '\0';
    if (len == 0) {
        PRINT("Usage: runback <command> [args]");
        return;
    }
    if (line[len - 1] != '&')
        cmd_scat(line, " &", CMD_MAX_LINE);

    ctx->dispatch_line(ctx->shell_extra, line);
}
