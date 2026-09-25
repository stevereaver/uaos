/* cmd_dmesg.c — C:dmesg — dump/filter the kernel log ring buffer
 *
 * Prints the contents of the klog ring buffer (kernel/klog).  Because it
 * is an ordinary shell command it also works over telnetd sessions.
 *
 *   dmesg                    dump the whole ring buffer
 *   dmesg <subsys> [...]     only the named subsystem(s)
 *   dmesg <level>  [...]     only entries at the given level(s)
 *   dmesg clear              discard all entries
 *
 * Subsystem and level filters may be combined, e.g. "dmesg dhcp dns warn".
 */

#include "cmd_internal.h"
#include "../klog/klog.h"

void Cmd_Dmesg(NativeCmdCtx *ctx, const char *args)
{
    uint32_t subsys_mask = 0;   /* 0 => all subsystems                 */
    uint32_t level_mask  = 0;   /* 0 => all levels                     */
    int had_error = 0;

    const char *p = args;
    while (p && *p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        int tlen = (int)(p - start);
        if (tlen <= 0 || tlen >= 24) { had_error = 1; continue; }

        char tok[24];
        for (int i = 0; i < tlen; i++) tok[i] = start[i];
        tok[tlen] = '\0';

        if (cmd_seq_ci(tok, "clear")) {
            klog_ring_clear();
            PRINT("klog ring buffer cleared.");
            continue;
        }

        int idx = klog_subsys_find(tok);
        if (idx >= 0) {
            subsys_mask |= (1u << idx);
            continue;
        }

        int lvl = klog_level_find(tok);
        if (lvl >= 0) {
            level_mask |= (1u << lvl);
            continue;
        }

        {
            char line[CMD_MAX_LINE];
            cmd_scopy(line, "dmesg: unknown filter '", CMD_MAX_LINE);
            cmd_scat(line, tok, CMD_MAX_LINE);
            cmd_scat(line, "'", CMD_MAX_LINE);
            PRINT(line);
        }
        had_error = 1;
    }

    if (had_error) {
        PRINT("Usage: dmesg [<subsys>|<level>|clear] ...");
        PRINT("       e.g. dmesg dhcp   dmesg warn   dmesg dhcp dns debug");
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 10);
        return;
    }

    uint32_t n = klog_ring_count();
    if (n == 0) {
        PRINT("(klog ring buffer is empty)");
        return;
    }

    for (uint32_t i = 0; i < n; i++) {
        int subsys, level;
        const char *text;
        if (!klog_ring_get(i, &subsys, &level, &text))
            break;
        if (subsys_mask && !(subsys_mask & (1u << subsys)))
            continue;
        if (level_mask && !(level_mask & (1u << level)))
            continue;
        PRINT(text);
    }
}
