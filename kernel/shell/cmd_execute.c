/* cmd_execute.c — C:execute — run a script file line by line */

#include "cmd_internal.h"

#define MAX_SCRIPT_SIZE 65536
#define MAX_EXEC_ARGS 9
static char g_exec_buf[MAX_SCRIPT_SIZE];

/* Scan the script text for a .key template declaration and copy the
 * template spec (everything after ".key ") into out[max].  Returns 1 if
 * a .key line was found, 0 otherwise.  Matching is case-insensitive and
 * tolerates leading whitespace. */
static int exec_find_key(const char *script, char *out, int max)
{
    const char *p = script;
    while (*p) {
        /* Skip leading whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '.') {
            p++;
            const char *kw = "key";
            int match = 1;
            int ki = 0;
            while (kw[ki]) {
                char c = p[ki];
                if (c >= 'A' && c <= 'Z') c += 32;
                if (c != kw[ki]) { match = 0; break; }
                ki++;
            }
            if (match && (p[ki] == ' ' || p[ki] == '\t' || p[ki] == '\0')) {
                p += ki;
                while (*p == ' ' || *p == '\t') p++;
                int oi = 0;
                while (*p && *p != '\n' && *p != '\r' && oi < max - 1)
                    out[oi++] = *p++;
                out[oi] = '\0';
                return 1;
            }
        }
        /* Skip to end of line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return 0;
}

/* Build the value string for a template item, suitable for assignment to
 * the corresponding $n positional variable.  Switches yield "1" when
 * present, "" otherwise.  /M items join all collected values with spaces.
 * /F and regular items use .value directly. */
static void exec_item_value(const CmdTemplateItem *it, char *out, int max)
{
    if (it->sw) {
        cmd_scopy(out, it->present ? "1" : "", max);
        return;
    }
    if (it->multiple) {
        out[0] = '\0';
        for (int i = 0; i < it->value_count; i++) {
            if (i > 0) cmd_scat(out, " ", max);
            cmd_scat(out, it->values[i], max);
        }
        return;
    }
    cmd_scopy(out, it->present ? it->value : "", max);
}

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
    char raw_args[CMD_MAX_LINE];
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

    /* Capture the raw argument string (everything after the filename) for
     * template matching and for $*. */
    while (*p == ' ' || *p == '\t') p++;
    cmd_scopy(raw_args, p, CMD_MAX_LINE);

    /* Build $* (all args) and split into $1..$9 (raw positional fallback) */
    arg_all[0] = '\0';
    int arg_count = 0;
    p = raw_args;
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

    /* --- Template-aware argument binding --- */
    /* If the script contains a .key declaration, parse it with the
     * AmigaDOS template parser and match the raw arguments against it.
     * The positional $1..$9 variables are then set in template-item
     * order so that <argname> references (which map names to positions)
     * resolve correctly, including /K keyword args passed as
     * name=value or name value.  Falls back to raw positional splitting
     * when there is no .key or the match fails. */
    char key_spec[CMD_MAX_LINE];
    int use_template = 0;
    CmdTemplateResult tr;

    if (exec_find_key(g_exec_buf, key_spec, CMD_MAX_LINE) && key_spec[0]) {
        CmdTemplate_Parse(key_spec, &tr);
        if (!tr.error[0]) {
            CmdTemplate_MatchArgs(&tr, raw_args);
            if (!tr.error[0]) {
                use_template = 1;
            } else {
                /* Template match failed — warn but continue with raw
                 * positional fallback so the script still runs. */
                char msg[CMD_MAX_LINE];
                cmd_scopy(msg, "execute: ", CMD_MAX_LINE);
                cmd_scat(msg, tr.error, CMD_MAX_LINE);
                PRINT(msg);
            }
        }
    }

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

        /* Set $* (always the raw argument string) */
        ctx->set_env(ctx->shell_extra, "*", arg_all);

        if (use_template) {
            /* Set $1..$9 in template-item order.  Each item maps to the
             * positional variable matching its index, so <argname> (which
             * resolves name -> position -> $n) gets the right value. */
            int nitems = tr.count;
            if (nitems > MAX_EXEC_ARGS) nitems = MAX_EXEC_ARGS;
            for (int n = 0; n < nitems; n++) {
                char varname[3] = { '1' + n, '\0' };
                char val[CMD_MAX_PATH];
                exec_item_value(&tr.items[n], val, CMD_MAX_PATH);
                ctx->set_env(ctx->shell_extra, varname, val);
            }
            /* Clear unused $n variables */
            for (int n = nitems; n < MAX_EXEC_ARGS; n++) {
                char varname[3] = { '1' + n, '\0' };
                ctx->set_env(ctx->shell_extra, varname, "");
            }
        } else {
            /* Raw positional fallback */
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
