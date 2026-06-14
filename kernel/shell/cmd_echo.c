/* cmd_echo.c — C:echo — print text to the shell */

#include "cmd_internal.h"

void Cmd_Echo(NativeCmdCtx *ctx, const char *args)
{
    if (!args) args = "";
    int noline = 0;
    int len = cmd_slen(args);

    /* Check if last token is NOLINE */
    if (len > 0) {
        const char *p = args + len - 1;
        while (p > args && *p == ' ') p--;
        const char *end = p + 1;
        while (p > args && *p != ' ') p--;
        const char *start = (*p == ' ') ? p + 1 : p;
        int toklen = (int)(end - start);
        if (toklen == 6) {
            if ((start[0]=='N'||start[0]=='n') && (start[1]=='O'||start[1]=='o') &&
                (start[2]=='L'||start[2]=='l') && (start[3]=='I'||start[3]=='i') &&
                (start[4]=='N'||start[4]=='n') && (start[5]=='E'||start[5]=='e')) {
                noline = 1;
                len = (int)(start - args);
                while (len > 0 && args[len-1] == ' ') len--;
            }
        }
    }

    if (len <= 0) {
        if (!noline) PRINT("");
        return;
    }

    char text[CMD_MAX_LINE];
    int i = 0;
    while (i < len && i < CMD_MAX_LINE - 1) {
        text[i] = args[i];
        i++;
    }
    text[i] = '\0';

    if (noline && ctx->print_raw) {
        ctx->print_raw(ctx->shell, text);
    } else {
        PRINT(text);
    }
}
