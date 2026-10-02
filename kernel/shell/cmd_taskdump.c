/* cmd_taskdump.c — C:taskdump — live task saved-frame dump (UAOS-195)
 *
 *   taskdump                 one-line summary of every live task
 *   taskdump TASK=name       full saved-frame decode for one task
 *   taskdump FULL            one-line + frame decode for all tasks
 *
 * Shows the parked interrupt frame (native_rsp -> rip/cs/rflags/regs)
 * the scheduler will resume into — the state UAOS-180/181 corrupted.
 */

#include "cmd_internal.h"
#include "../exec/task.h"

static void td_emit(void *ctx, const char *line)
{
    CMD_PRINT((NativeCmdCtx *)ctx, line);
}

void Cmd_Taskdump(NativeCmdCtx *ctx, const char *args)
{
    char name[64];
    name[0] = '\0';
    int full = 0;

    /* parse optional TASK=name and/or bare name and/or FULL */
    if (args && *args) {
        char work[CMD_MAX_LINE];
        cmd_scopy(work, args, sizeof(work));
        char *p = work;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            char *tok = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
            if (cmd_seq_ci(tok, "full")) { full = 1; continue; }
            if (tok[0] && (cmd_seq_ci(tok, "task=") || 0)) { /* fallthrough */ }
            /* TASK=name or bare name */
            const char *v = tok;
            if ((tok[0]=='T'||tok[0]=='t') && cmd_slen(tok) > 5) {
                char t2[8]; int i = 0;
                while (i < 5 && tok[i]) { t2[i] = tok[i]; i++; }
                t2[i] = 0;
                if (cmd_seq_ci(t2, "task=")) v = tok + 5;
            }
            cmd_scopy(name, v, sizeof(name));
        }
    }
    if (name[0]) full = 1;

    PRINT("  #  name                 type   state    pri cpu_t sw   stackpk");
    Task_DiagDump(ctx, td_emit, name[0] ? name : NULL, full);
}
