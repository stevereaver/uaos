/* cmd_execute.c — C:execute — run a script file line by line */

#include "cmd_internal.h"

#define MAX_SCRIPT_SIZE 65536
#define MAX_EXEC_ARGS 9
static char g_exec_buf[MAX_SCRIPT_SIZE];

void Cmd_Execute(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: execute <script> [args...]");
        PRINT("Executes a script file line by line.");
        return;
    }

    /* Parse script filename and arguments */
    char script_path[CMD_MAX_PATH];
    char arg_all[CMD_MAX_LINE];
    char *arg_tokens[MAX_EXEC_ARGS];
    char arg_bufs[MAX_EXEC_ARGS][CMD_MAX_PATH];
    char saved_vars[MAX_EXEC_ARGS][CMD_MAX_PATH];
    char saved_all[CMD_MAX_PATH];

    /* Skip leading whitespace */
    const char *p = args;
    while (*p == ' ' || *p == '\t') p++;

    /* Extract script filename (first token) */
    int i = 0;
    while (*p && *p != ' ' && *p != '\t' && i < CMD_MAX_PATH - 1) {
        script_path[i++] = *p++;
    }
    script_path[i] = '\0';

    /* Build absolute path */
    char full_path[CMD_MAX_PATH];
    cmd_make_abs(ctx->cwd, script_path, full_path, CMD_MAX_PATH);

    /* Extract remaining arguments */
    while (*p == ' ' || *p == '\t') p++;

    /* Build $* (all args) and split into $1..$9 */
    arg_all[0] = '\0';
    int arg_count = 0;
    while (*p && arg_count < MAX_EXEC_ARGS) {
        /* Copy one token */
        int j = 0;
        while (*p && *p != ' ' && *p != '\t' && j < CMD_MAX_PATH - 1) {
            arg_bufs[arg_count][j++] = *p++;
        }
        arg_bufs[arg_count][j] = '\0';
        arg_tokens[arg_count] = arg_bufs[arg_count];

        /* Append to $* */
        if (arg_all[0]) cmd_scat(arg_all, " ", CMD_MAX_LINE);
        cmd_scat(arg_all, arg_bufs[arg_count], CMD_MAX_LINE);
        arg_count++;

        while (*p == ' ' || *p == '\t') p++;
    }

    /* Open script file */
    VfsFile fh;
    if (!VFS_Open(&fh, full_path, VFS_READ)) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "Cannot open: ", CMD_MAX_LINE);
        cmd_scat(msg, full_path, CMD_MAX_LINE);
        PRINT(msg);
        return;
    }

    uint32_t size = VFS_Size(&fh);
    if (size == 0 || size >= MAX_SCRIPT_SIZE) {
        PRINT("Script empty or too large (max 64KB)");
        VFS_Close(&fh);
        return;
    }

    uint32_t nread = VFS_Read(&fh, (uint8_t *)g_exec_buf, size);
    g_exec_buf[nread] = '\0';
    VFS_Close(&fh);

    /* Save existing argument variables */
    if (ctx->get_env && ctx->set_env && ctx->shell_extra) {
        /* Save $* */
        saved_all[0] = '\0';
        ctx->get_env(ctx->shell_extra, "*", saved_all, sizeof(saved_all));

        /* Save $1..$9 */
        for (int n = 0; n < MAX_EXEC_ARGS; n++) {
            char varname[3] = { '1' + n, '\0' };
            saved_vars[n][0] = '\0';
            ctx->get_env(ctx->shell_extra, varname, saved_vars[n], CMD_MAX_PATH);
        }

        /* Set new argument variables */
        ctx->set_env(ctx->shell_extra, "*", arg_all);
        for (int n = 0; n < arg_count; n++) {
            char varname[3] = { '1' + n, '\0' };
            ctx->set_env(ctx->shell_extra, varname, arg_tokens[n]);
        }
        /* Clear unused $n variables */
        for (int n = arg_count; n < MAX_EXEC_ARGS; n++) {
            char varname[3] = { '1' + n, '\0' };
            ctx->set_env(ctx->shell_extra, varname, "");
        }
    }

    /* Run the script */
    if (ctx->run_script && ctx->shell_extra) {
        ctx->run_script(ctx->shell_extra, g_exec_buf);
    } else {
        /* Fallback: line-by-line dispatch without flow control */
        char line[CMD_MAX_LINE];
        const char *lp = g_exec_buf;
        while (*lp) {
            int li = 0;
            while (*lp && *lp != '\n' && li < CMD_MAX_LINE - 1) {
                if (*lp != '\r') line[li++] = *lp;
                lp++;
            }
            if (*lp == '\n') lp++;
            line[li] = '\0';

            const char *lcmd = line;
            while (*lcmd == ' ' || *lcmd == '\t') lcmd++;
            if (!*lcmd || *lcmd == ';') continue;

            if (ctx->dispatch_line && ctx->shell_extra)
                ctx->dispatch_line(ctx->shell_extra, line);
            else
                PRINT(line);
        }
    }

    /* Restore saved argument variables */
    if (ctx->get_env && ctx->set_env && ctx->shell_extra) {
        ctx->set_env(ctx->shell_extra, "*", saved_all);
        for (int n = 0; n < MAX_EXEC_ARGS; n++) {
            char varname[3] = { '1' + n, '\0' };
            ctx->set_env(ctx->shell_extra, varname, saved_vars[n]);
        }
    }
}
