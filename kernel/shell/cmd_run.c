/* cmd_run.c — C:run — execute a command line detached
 *
 * AmigaOS semantics: run takes the rest of the line, resolves it like a
 * normal command (built-ins, resident list, native C: commands, PATH/cwd
 * binaries — NATIVE, M68k and X64) and lets it execute without blocking
 * the shell.  Implemented by re-dispatching the line through the shell's
 * background-job mechanism, equivalent to appending '&'.
 *
 * If run's own stdout was redirected (e.g. "run >NIL: C:ntpd"), the child
 * inherits it: ctx->out_redirect carries the enclosing redirect spec and
 * is re-encoded into the dispatched line.
 */

#include "cmd_internal.h"
#include "../../emulation/uaos_emu.h"

void Cmd_Run(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: run <command> [args]");
        PRINT("Runs the command detached, in the background.");
        return;
    }

    if (!ctx->dispatch_line || !ctx->shell_extra) {
        /* No shell job queue — fall back to the embedded binary registry. */
        UAOS_Emu_SetCwd(ctx->cwd);
        UAOS_Emu_RunByName(args, ctx->shell, (UAOS_PrintFn)ctx->print);
        return;
    }

    char line[CMD_MAX_LINE];
    cmd_scopy(line, args, CMD_MAX_LINE);

    /* Normalise: strip trailing spaces and any explicit '&' — re-added below
     * so a propagated redirect lands before it. */
    int len = cmd_slen(line);
    while (len > 0 && line[len - 1] == ' ') line[--len] = '\0';
    if (len > 0 && line[len - 1] == '&') {
        line[--len] = '\0';
        while (len > 0 && line[len - 1] == ' ') line[--len] = '\0';
    }
    if (len == 0) {
        PRINT("Usage: run <command> [args]");
        return;
    }

    if (ctx->out_redirect && ctx->out_redirect[0]) {
        cmd_scat(line, " ", CMD_MAX_LINE);
        cmd_scat(line, ctx->out_redirect, CMD_MAX_LINE);
    }
    cmd_scat(line, " &", CMD_MAX_LINE);

    ctx->dispatch_line(ctx->shell_extra, line);
}
