/* cmd_changetaskpri.c — C:changetaskpri — change a task's priority
 *
 * Syntax: changetaskpri <priority> [TASK <name>]
 *         changetaskpri 5 TASK Shell_1
 *
 * Sets the scheduling priority of the named task (or the current task if
 * no name is given).  Valid range is -128 to 127, matching the AmigaOS
 * ln_Pri field.  A higher number means more CPU time.
 *
 * Return codes: 0 = success, 5 = task not found, 20 = bad priority.
 */

#include "cmd_internal.h"
#include "../exec/task.h"

/* Parse a signed decimal integer from p into *out.  Returns bytes consumed. */
static int ctp_parse_int(const char *p, int *out)
{
    int neg = 0, v = 0, n = 0;
    if (*p == '-') { neg = 1; p++; n++; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; n++; }
    *out = neg ? -v : v;
    return n;
}

void Cmd_ChangeTaskPri(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: changetaskpri <priority> [TASK <name>]");
        PRINT("  priority  Signed integer -128..127 (higher = more CPU)");
        PRINT("  TASK      Name of the task to adjust (default: current)");
        return;
    }

    int   pri = 0;
    char  taskname[32] = {0};
    const char *p = args;

    while (*p == ' ') p++;

    /* First token: priority value */
    int consumed = ctp_parse_int(p, &pri);
    if (consumed == 0) {
        PRINT("changetaskpri: priority must be a number.");
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 20);
        return;
    }
    p += consumed;

    if (pri < -128 || pri > 127) {
        PRINT("changetaskpri: priority out of range (-128 to 127).");
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 20);
        return;
    }

    /* Optional TASK <name> keyword */
    while (*p == ' ') p++;
    if ((p[0]=='T'||p[0]=='t') && (p[1]=='A'||p[1]=='a') &&
        (p[2]=='S'||p[2]=='s') && (p[3]=='K'||p[3]=='k') &&
        (p[4]==' ' || p[4]=='\0')) {
        p += 4;
        while (*p == ' ') p++;
        int i = 0;
        while (*p && *p != ' ' && i < 31) taskname[i++] = *p++;
        taskname[i] = '\0';
    }

    /* Use context callback if available */
    if (ctx->change_task_pri) {
        int ok = ctx->change_task_pri(ctx->shell_extra,
                                      taskname[0] ? taskname : NULL,
                                      (int8_t)pri);
        if (!ok) {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "changetaskpri: task not found: ", CMD_MAX_LINE);
            cmd_scat(msg, taskname, CMD_MAX_LINE);
            PRINT(msg);
            if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 5);
            return;
        }
    } else {
        /* Fallback: direct Task_FindByName */
        UaosTask *t = taskname[0] ? Task_FindByName(taskname) : Task_Current();
        if (!t) {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "changetaskpri: task not found: ", CMD_MAX_LINE);
            cmd_scat(msg, taskname, CMD_MAX_LINE);
            PRINT(msg);
            if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 5);
            return;
        }
        t->ln_Pri = (int8_t)pri;
    }

    char msg[CMD_MAX_LINE];
    cmd_scopy(msg, "Priority set to ", CMD_MAX_LINE);
    if (pri < 0) {
        cmd_scat(msg, "-", CMD_MAX_LINE);
        char num[8];
        cmd_uint_to_dec((uint32_t)(-pri), num, 8);
        cmd_scat(msg, num, CMD_MAX_LINE);
    } else {
        char num[8];
        cmd_uint_to_dec((uint32_t)pri, num, 8);
        cmd_scat(msg, num, CMD_MAX_LINE);
    }
    if (taskname[0]) {
        cmd_scat(msg, " for task ", CMD_MAX_LINE);
        cmd_scat(msg, taskname, CMD_MAX_LINE);
    }
    PRINT(msg);
}
