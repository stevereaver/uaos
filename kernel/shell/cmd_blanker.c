/* cmd_blanker.c — Blanker command
 *
 * Toggles the screen blanker commodity state.
 * Usage: blanker [on|off|sleep|wake|timeout <secs>]
 */

#include "native_cmd.h"

void Cmd_Blanker(NativeCmdCtx *ctx, const char *args)
{
    extern int  Cx_FindByName(const char *);
    extern void Cx_Enable(int);
    extern void Cx_Disable(int);
    extern void Cx_Sleep(int);
    extern void Cx_Wake(int);
    extern void Blanker_SetTimeout(int);
    extern int  Blanker_GetTimeout(void);

    int idx = Cx_FindByName("Blanker");
    if (idx < 0) {
        if (ctx->print) ctx->print(ctx->shell, "Blanker commodity not registered.");
        return;
    }

    /* Parse simple args */
    if (!args || !args[0]) {
        /* No args — cycle state */
        extern void Cx_CycleState(int);
        Cx_CycleState(idx);
        if (ctx->print) ctx->print(ctx->shell, "Blanker state cycled.");
        return;
    }

    /* Check for known keywords */
    int i = 0;
    while (args[i] == ' ') i++;

    /* "on" */
    if (args[i] == 'o' && args[i+1] == 'n' && (args[i+2] == '\0' || args[i+2] == ' ')) {
        Cx_Enable(idx);
        if (ctx->print) ctx->print(ctx->shell, "Blanker enabled.");
        return;
    }
    /* "off" */
    if (args[i] == 'o' && args[i+1] == 'f' && args[i+2] == 'f' && (args[i+3] == '\0' || args[i+3] == ' ')) {
        Cx_Disable(idx);
        if (ctx->print) ctx->print(ctx->shell, "Blanker disabled.");
        return;
    }
    /* "sleep" */
    if (args[i] == 's' && args[i+1] == 'l' && (args[i+2] == '\0' || args[i+2] == ' ')) {
        Cx_Sleep(idx);
        if (ctx->print) ctx->print(ctx->shell, "Blanker sleeping.");
        return;
    }
    /* "wake" */
    if (args[i] == 'w' && args[i+1] == 'a' && (args[i+2] == '\0' || args[i+2] == ' ')) {
        Cx_Wake(idx);
        if (ctx->print) ctx->print(ctx->shell, "Blanker woken.");
        return;
    }
    /* "timeout" */
    if (args[i] == 't' && args[i+1] == 'i' && args[i+2] == 'm') {
        int j = i + 7;
        while (args[j] == ' ') j++;
        int val = 0;
        while (args[j] >= '0' && args[j] <= '9') {
            val = val * 10 + (args[j] - '0');
            j++;
        }
        if (val > 0) {
            Blanker_SetTimeout(val);
            char msg[64];
            extern void pw_int_str(char *, int);
            /* Build message manually */
            int mi = 0;
            const char *s = "Blanker timeout set to ";
            while (*s && mi < 60) msg[mi++] = *s++;
            /* Convert val to string */
            char num[12];
            int ni = 0;
            if (val == 0) num[ni++] = '0';
            while (val > 0) { num[ni++] = '0' + (val % 10); val /= 10; }
            while (ni > 0 && mi < 62) msg[mi++] = num[--ni];
            msg[mi] = '\0';
            if (ctx->print) ctx->print(ctx->shell, msg);
        }
        return;
    }

    /* Default: cycle */
    extern void Cx_CycleState(int);
    Cx_CycleState(idx);
    if (ctx->print) ctx->print(ctx->shell, "Blanker state cycled.");
}
