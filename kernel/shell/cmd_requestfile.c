/* cmd_requestfile.c — C:requestfile — text-mode file path requester
 *
 * Syntax:
 *   requestfile [TITLE <title>] [DRAWER <dir>] [FILE <default>] [PATTERN <pat>]
 *               [PUBSCREEN <name>]
 *
 * Presents an interactive file-path prompt in the shell window (or, in a
 * future Intuition-capable build, a proper ASL file requester).  The chosen
 * path is stored in the environment variable RESULT.
 *
 * AmigaDOS returns code 5 (WARN) if the user cancels; returns 0 on success.
 *
 * Usage in scripts:
 *   requestfile TITLE "Choose config" DRAWER "SYS:Prefs" PATTERN "#?.prefs"
 *   if $RESULT eq "" then quit
 *   execute $RESULT
 */

#include "cmd_internal.h"

/* Parse a keyword=value pair starting at *pp.  Returns 1 if matched. */
static int rf_kw(const char **pp, const char *kw, char *out, int max)
{
    const char *p = *pp;
    int kl = 0;
    while (kw[kl]) kl++;

    /* Case-insensitive keyword match */
    for (int i = 0; i < kl; i++) {
        char a = p[i]; if (a >= 'A' && a <= 'Z') a += 32;
        char b = kw[i]; if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    if (p[kl] != ' ' && p[kl] != '\0') return 0;

    p += kl;
    while (*p == ' ') p++;

    int i = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && i < max - 1) out[i++] = *p++;
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && i < max - 1) out[i++] = *p++;
    }
    out[i] = '\0';
    *pp = p;
    return 1;
}

void Cmd_RequestFile(NativeCmdCtx *ctx, const char *args)
{
    char title[64]   = "Select a file";
    char drawer[64]  = "";
    char file[64]    = "";
    char pattern[32] = "#?";

    /* Parse keyword arguments */
    const char *p = args ? args : "";
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        if (rf_kw(&p, "TITLE",     title,   sizeof(title)))   continue;
        if (rf_kw(&p, "DRAWER",    drawer,  sizeof(drawer)))  continue;
        if (rf_kw(&p, "FILE",      file,    sizeof(file)))    continue;
        if (rf_kw(&p, "PATTERN",   pattern, sizeof(pattern))) continue;
        if (rf_kw(&p, "PUBSCREEN", title,   sizeof(title)))   continue; /* ignored */

        /* Skip unknown token */
        while (*p && *p != ' ') p++;
    }

    /* Display the requester as a text prompt */
    PRINT("----------------------------------------");
    PRINT(title);
    PRINT("----------------------------------------");

    if (drawer[0]) {
        char line[CMD_MAX_LINE];
        cmd_scopy(line, "  Drawer : ", CMD_MAX_LINE);
        cmd_scat(line, drawer, CMD_MAX_LINE);
        PRINT(line);
    }
    if (pattern[0]) {
        char line[CMD_MAX_LINE];
        cmd_scopy(line, "  Pattern: ", CMD_MAX_LINE);
        cmd_scat(line, pattern, CMD_MAX_LINE);
        PRINT(line);
    }
    if (file[0]) {
        char line[CMD_MAX_LINE];
        cmd_scopy(line, "  Default: ", CMD_MAX_LINE);
        cmd_scat(line, file, CMD_MAX_LINE);
        PRINT(line);
    }
    PRINT("Enter path (empty to cancel):");

    /* Build default path for the prompt */
    char default_path[CMD_MAX_PATH];
    default_path[0] = '\0';
    if (drawer[0]) {
        cmd_scopy(default_path, drawer, CMD_MAX_PATH);
        int dl = cmd_slen(default_path);
        if (dl > 0 && default_path[dl - 1] != ':' && default_path[dl - 1] != '/') {
            cmd_scat(default_path, "/", CMD_MAX_PATH);
        }
    }
    if (file[0]) cmd_scat(default_path, file, CMD_MAX_PATH);

    if (ctx->set_ask_mode)
        ctx->set_ask_mode(ctx->shell_extra,
                          default_path[0] ? default_path : "File: ");

    char chosen[CMD_MAX_PATH];
    chosen[0] = '\0';
    int n = 0;

    if (ctx->read_line)
        n = ctx->read_line(ctx->shell_extra, chosen, sizeof(chosen));

    /* If user entered nothing but there is a default, use it */
    if (n == 0 && default_path[0]) {
        cmd_scopy(chosen, default_path, sizeof(chosen));
        n = cmd_slen(chosen);
    }

    /* Store result in $RESULT */
    if (ctx->set_env)
        ctx->set_env(ctx->shell_extra, "RESULT", chosen);

    if (ctx->dispatch_line) {
        char setcmd[CMD_MAX_PATH + 16];
        cmd_scopy(setcmd, "set RESULT ", sizeof(setcmd));
        cmd_scat(setcmd, chosen, sizeof(setcmd));
        ctx->dispatch_line(ctx->shell_extra, setcmd);
    }

    /* Return 5 (WARN) if user cancelled (empty result), 0 otherwise */
    if (ctx->set_rc)
        ctx->set_rc(ctx->shell_extra, (n == 0) ? 5 : 0);
}
