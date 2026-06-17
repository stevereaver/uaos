/* cmd_requestchoice.c — C:requestchoice — text-mode choice requester
 *
 * Syntax:
 *   requestchoice <title> <body> <button1> [<button2> ...]
 *   requestchoice "Select action" "Proceed?" "Yes" "No" "Cancel"
 *
 * Displays a numbered menu in the shell and waits for the user to type the
 * number of their choice.  The result (1-based button number) is stored in
 * the environment variable RC and also set as the command return code.
 *
 * The classic AmigaOS requestchoice would open an Intuition requester window;
 * since UAOS does not yet have a modal Intuition dialog layer, we fall back
 * to an interactive text prompt inside the shell that mirrors the AmigaOS
 * semantics exactly: RC holds the button number (counting from 1 for the
 * left-most button, 0 if the user dismisses without choosing).
 *
 * Usage in scripts:
 *   requestchoice "Quit?" "Are you sure?" "Yes" "No"
 *   if $RC eq 1 then quit
 */

#include "cmd_internal.h"

/* Parse one quoted or unquoted token from *pp.  Returns length written. */
static int rc_parse_token(const char **pp, char *out, int max)
{
    const char *p = *pp;
    while (*p == ' ') p++;
    if (!*p) { *pp = p; return 0; }

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
    return i;
}

#define MAX_BUTTONS 8

void Cmd_RequestChoice(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: requestchoice <title> <body> <btn1> [<btn2> ...]");
        PRINT("Result is stored in $RC (button number, 1-based; 0 = cancelled).");
        return;
    }

    const char *p = args;
    char title[64], body[128];
    char buttons[MAX_BUTTONS][32];
    int  nb = 0;

    rc_parse_token(&p, title, sizeof(title));
    rc_parse_token(&p, body,  sizeof(body));
    while (*p && nb < MAX_BUTTONS) {
        int len = rc_parse_token(&p, buttons[nb], 32);
        if (len > 0) nb++;
    }

    if (nb == 0) {
        PRINT("requestchoice: no buttons specified.");
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 20);
        return;
    }

    /* Display the requester as a text box */
    PRINT("----------------------------------------");
    PRINT(title);
    PRINT("----------------------------------------");
    PRINT(body);
    PRINT("");

    for (int i = 0; i < nb; i++) {
        char line[48];
        char num[8];
        cmd_uint_to_dec((uint32_t)(i + 1), num, 8);
        cmd_scopy(line, "  [", 48);
        cmd_scat(line, num, 48);
        cmd_scat(line, "] ", 48);
        cmd_scat(line, buttons[i], 48);
        PRINT(line);
    }
    PRINT("----------------------------------------");

    /* Read user's choice */
    int choice = 0;
    if (ctx->set_ask_mode) ctx->set_ask_mode(ctx->shell_extra, "Choice: ");
    if (ctx->read_line) {
        char buf[16];
        int n = ctx->read_line(ctx->shell_extra, buf, sizeof(buf));
        if (n > 0 && buf[0] >= '1' && buf[0] <= '0' + nb) {
            choice = buf[0] - '0';
        }
    }

    /* Store result in $RC */
    char numstr[8];
    cmd_uint_to_dec((uint32_t)choice, numstr, 8);
    if (ctx->set_env) {
        ctx->set_env(ctx->shell_extra, "RC", numstr);
    }
    if (ctx->dispatch_line) {
        char setcmd[32];
        cmd_scopy(setcmd, "set RC ", 32);
        cmd_scat(setcmd, numstr, 32);
        ctx->dispatch_line(ctx->shell_extra, setcmd);
    }
    if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, choice);
}
