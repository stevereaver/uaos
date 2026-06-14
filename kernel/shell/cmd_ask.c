/* cmd_ask.c — C:ask — interactive prompt for user input
 *
 * Syntax: ask <prompt> [default]
 *
 * Displays the prompt to the user, waits for input, and sets
 * the environment variable ASK to the user's response.
 * If the user presses Enter without typing anything, the default
 * value is used (if provided).
 *
 * Example:
 *   ask "Continue? (y/n)" n
 *   if "$ASK" eq "y" then echo "Continuing..."
 */

#include "cmd_internal.h"

void Cmd_Ask(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: ask <prompt> [default]");
        PRINT("Displays prompt and waits for user input.");
        PRINT("Result is stored in $ASK environment variable.");
        return;
    }

    /* Parse arguments: prompt and optional default */
    char prompt[128];
    char default_val[64] = "";

    /* Extract prompt (first argument) */
    const char *p = args;
    int i = 0;

    /* Skip leading spaces */
    while (*p == ' ' || *p == '\t') p++;

    /* Check if prompt is quoted */
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && i < 127) {
            prompt[i++] = *p++;
        }
        if (*p == '"') p++;
    } else {
        /* Unquoted prompt - read until end or space before default */
        while (*p && *p != ' ' && *p != '\t' && i < 127) {
            prompt[i++] = *p++;
        }
    }
    prompt[i] = '\0';

    /* Skip spaces after prompt */
    while (*p == ' ' || *p == '\t') p++;

    /* Extract default value if provided */
    if (*p) {
        int j = 0;
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && j < 63) {
                default_val[j++] = *p++;
            }
        } else {
            while (*p && *p != ' ' && *p != '\t' && j < 63) {
                default_val[j++] = *p++;
            }
        }
        default_val[j] = '\0';
    }

    /* Set ask mode with custom prompt */
    if (ctx->set_ask_mode) {
        ctx->set_ask_mode(ctx->shell_extra, prompt);
    }

    /* Read user input */
    char response[128];
    int len = 0;

    if (ctx->read_line) {
        len = ctx->read_line(ctx->shell_extra, response, sizeof(response));
    } else {
        PRINT("Error: read_line callback not available");
        return;
    }

    /* Use default if user entered nothing */
    if (len == 0 && default_val[0]) {
        cmd_scopy(response, default_val, sizeof(response));
        len = cmd_slen(response);
    }

    /* Set ASK environment variable */
    /* We need to dispatch a 'set' command to set the variable */
    char set_cmd[256];
    cmd_scopy(set_cmd, "set ASK ", sizeof(set_cmd));
    cmd_scat(set_cmd, response, sizeof(set_cmd));

    if (ctx->dispatch_line) {
        ctx->dispatch_line(ctx->shell_extra, set_cmd);
    }

    /* Also set it in ENV: for persistence across commands */
    /* This is done by creating/updating ENV:ASK file */
}
