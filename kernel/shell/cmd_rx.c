/* cmd_rx.c — C:rx — run an ARexx program via Regina Rexx
 *
 * Provides the AmigaOS `rx` command by wrapping the Regina Rexx
 * interpreter (REXX:rexx).  Supports both inline programs:
 *   rx "say 'Hello'"
 * and file-based programs:
 *   rx myscript.rexx arg1 arg2
 *
 * For inline programs, the quoted string is written to a temp file
 * in T: and then dispatched to the interpreter.  For file-based
 * programs, the filename is passed directly.
 */

#include "cmd_internal.h"

void Cmd_Rx(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: rx <program> [args]  or  rx \"inline program\"");
        return;
    }

    /* Skip leading whitespace */
    while (*args == ' ') args++;
    if (!*args) {
        PRINT("Usage: rx <program> [args]");
        return;
    }

    char dispatch_buf[CMD_MAX_LINE];

    /* Check if the argument starts with a double-quote (inline program) */
    if (*args == '"') {
        /* Extract the inline program string between the outermost quotes */
        const char *start = args + 1;
        const char *end = start;
        while (*end && *end != '"') end++;
        if (!*end) {
            PRINT("rx: missing closing quote");
            return;
        }
        int prog_len = (int)(end - start);

        /* Any arguments after the closing quote */
        const char *rest = end + 1;
        while (*rest == ' ') rest++;

        /* Write the inline program to a temp file */
        VfsFile fh;
        if (!VFS_Open(&fh, "T:rx_temp.rexx", VFS_WRITE | VFS_CREATE | VFS_TRUNC)) {
            PRINT("rx: cannot create temp file T:rx_temp.rexx");
            return;
        }
        VFS_Write(&fh, (const uint8_t *)start, (uint32_t)prog_len);
        VFS_Write(&fh, (const uint8_t *)"\n", 1);
        VFS_Close(&fh);

        /* Dispatch: REXX:rexx T:rx_temp.rexx [rest] */
        cmd_scopy(dispatch_buf, "REXX:rexx T:rx_temp.rexx", CMD_MAX_LINE);
        if (*rest) {
            cmd_scat(dispatch_buf, " ", CMD_MAX_LINE);
            cmd_scat(dispatch_buf, rest, CMD_MAX_LINE);
        }

        if (ctx->dispatch_line && ctx->shell_extra) {
            ctx->dispatch_line(ctx->shell_extra, dispatch_buf);
        } else {
            PRINT("rx: shell dispatch not available");
        }

        /* Clean up temp file */
        VFS_Delete("T:rx_temp.rexx");

        /* Propagate return code */
        if (ctx->get_last_rc && ctx->shell_extra) {
            int rc = ctx->get_last_rc(ctx->shell_extra);
            if (ctx->set_rc && ctx->shell_extra)
                ctx->set_rc(ctx->shell_extra, rc);
        }
        return;
    }

    /* File-based program: check if it's a bare name (no path/extension).
     * If so, search REXX: and append .rexx if needed. */
    const char *p = args;
    int has_colon = 0, has_slash = 0, has_dot = 0;
    while (*p && *p != ' ') {
        if (*p == ':') has_colon = 1;
        if (*p == '/') has_slash = 1;
        if (*p == '.') has_dot = 1;
        p++;
    }
    int name_len = (int)(p - args);
    const char *file_args = p;
    while (*file_args == ' ') file_args++;

    char filename[CMD_MAX_PATH];

    if (!has_colon && !has_slash && !has_dot) {
        /* Bare name: try REXX:<name>.rexx */
        cmd_scopy(filename, "REXX:", CMD_MAX_PATH);
        cmd_scat(filename, args, CMD_MAX_PATH);
        cmd_scat(filename, ".rexx", CMD_MAX_PATH);

        /* Check if the file exists; if not, try without .rexx */
        VfsFile test;
        if (!VFS_Open(&test, filename, VFS_READ)) {
            cmd_scopy(filename, "REXX:", CMD_MAX_PATH);
            cmd_scat(filename, args, CMD_MAX_PATH);
            if (!VFS_Open(&test, filename, VFS_READ)) {
                /* Fall back to the bare name as typed */
                cmd_scopy(filename, args, CMD_MAX_PATH);
            } else {
                VFS_Close(&test);
            }
        } else {
            VFS_Close(&test);
        }
    } else {
        /* Has path/extension: use as-is */
        cmd_scopy(filename, args, CMD_MAX_PATH);
    }

    /* Dispatch: REXX:rexx <filename> [file_args] */
    cmd_scopy(dispatch_buf, "REXX:rexx ", CMD_MAX_LINE);
    cmd_scat(dispatch_buf, filename, CMD_MAX_LINE);
    if (*file_args) {
        cmd_scat(dispatch_buf, " ", CMD_MAX_LINE);
        cmd_scat(dispatch_buf, file_args, CMD_MAX_LINE);
    }

    if (ctx->dispatch_line && ctx->shell_extra) {
        ctx->dispatch_line(ctx->shell_extra, dispatch_buf);
    } else {
        PRINT("rx: shell dispatch not available");
    }

    /* Propagate return code */
    if (ctx->get_last_rc && ctx->shell_extra) {
        int rc = ctx->get_last_rc(ctx->shell_extra);
        if (ctx->set_rc && ctx->shell_extra)
            ctx->set_rc(ctx->shell_extra, rc);
    }
}
