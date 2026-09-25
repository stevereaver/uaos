/* cmd_klog.c — C:klog / C:debug — kernel log mask control
 *
 * Runtime control for the unified kernel logger (kernel/klog).  Each
 * subsystem has a severity threshold; a message is emitted only when its
 * level is at least as severe as the threshold.
 *
 *   klog                     list all subsystems and their levels
 *   klog <subsys>            show one subsystem's level
 *   klog <s>[,<s>...]=<lvl>  set level for one or more subsystems
 *   klog all=<lvl>           set every subsystem
 *
 * Levels: off error warn info debug trace
 * Example: klog vfs,net=debug
 */

#include "cmd_internal.h"
#include "../klog/klog.h"

static void klog_print_level(NativeCmdCtx *ctx, int subsys)
{
    char line[CMD_MAX_LINE];
    cmd_scopy(line, klog_subsys_name(subsys), CMD_MAX_LINE);
    cmd_scat(line, "=", CMD_MAX_LINE);
    cmd_scat(line, klog_level_name(klog_get_level(subsys)), CMD_MAX_LINE);
    PRINT(line);
}

static int klog_set_from_names(NativeCmdCtx *ctx,
                               const char *names, int nlen, int level)
{
    /* names is a comma-separated list, not NUL-terminated at nlen token end */
    int ok = 1;
    const char *p = names;
    while (p < names + nlen) {
        const char *start = p;
        while (p < names + nlen && *p != ',') p++;

        char sub[24];
        int sl = (int)(p - start);
        if (sl >= (int)sizeof(sub)) sl = (int)sizeof(sub) - 1;
        for (int i = 0; i < sl; i++) sub[i] = start[i];
        sub[sl] = '\0';

        if (cmd_seq_ci(sub, "all")) {
            klog_set_level(-1, level);
        } else {
            int idx = klog_subsys_find(sub);
            if (idx < 0) {
                char line[CMD_MAX_LINE];
                cmd_scopy(line, "klog: unknown subsystem '", CMD_MAX_LINE);
                cmd_scat(line, sub, CMD_MAX_LINE);
                cmd_scat(line, "'", CMD_MAX_LINE);
                PRINT(line);
                ok = 0;
            } else {
                klog_set_level(idx, level);
            }
        }
        if (*p == ',') p++;
    }
    return ok;
}

void Cmd_Klog(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Subsystem log levels (klog <s>[,<s>...]=<level>):");
        for (int i = 0; i < KLOG_NSUBSYS; i++)
            klog_print_level(ctx, i);
        return;
    }

    /* Process each space-separated token */
    const char *p = args;
    int had_error = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        int tlen = (int)(p - start);

        /* Look for '=' inside the token */
        const char *eq = NULL;
        for (const char *q = start; q < start + tlen; q++) {
            if (*q == '=') { eq = q; break; }
        }

        if (eq) {
            /* <names>=<level> */
            char lname[16];
            int ll = (int)(start + tlen - eq - 1);
            if (ll >= (int)sizeof(lname)) ll = (int)sizeof(lname) - 1;
            for (int i = 0; i < ll; i++) lname[i] = eq[1 + i];
            lname[ll] = '\0';

            int level = klog_level_find(lname);
            if (level < 0) {
                char line[CMD_MAX_LINE];
                cmd_scopy(line, "klog: unknown level '", CMD_MAX_LINE);
                cmd_scat(line, lname, CMD_MAX_LINE);
                cmd_scat(line, "'", CMD_MAX_LINE);
                PRINT(line);
                had_error = 1;
            } else {
                if (klog_set_from_names(ctx, start, (int)(eq - start), level))
                    PRINT("ok");
            }
        } else {
            /* Bare token: subsystem query */
            char sub[24];
            int sl = tlen < (int)sizeof(sub) - 1 ? tlen : (int)sizeof(sub) - 1;
            for (int i = 0; i < sl; i++) sub[i] = start[i];
            sub[sl] = '\0';

            int idx = klog_subsys_find(sub);
            if (idx < 0) {
                char line[CMD_MAX_LINE];
                cmd_scopy(line, "klog: unknown subsystem '", CMD_MAX_LINE);
                cmd_scat(line, sub, CMD_MAX_LINE);
                cmd_scat(line, "'", CMD_MAX_LINE);
                PRINT(line);
                had_error = 1;
            } else {
                klog_print_level(ctx, idx);
            }
        }
    }

    if (had_error) {
        PRINT("Usage: klog [<subsys>[,<subsys>...]=<level>]");
        PRINT("       levels: off error warn info debug trace");
        PRINT("       e.g.    klog vfs,net=debug   klog all=off");
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 10);
    }
}
