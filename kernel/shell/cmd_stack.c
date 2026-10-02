/* cmd_stack.c — C:stack — stack sizes + high-water marks (UAOS-212)
 *
 *   stack           sizes + per-task peak usage from the 0xA5 fill
 *                   watermark, and canary status
 */

#include "cmd_internal.h"
#include "../exec/task.h"

void Cmd_Stack(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    char line[CMD_MAX_LINE], num[24];

    PRINT("Native task stack: 32 KB per task (fill 0xA5, base canary)");
    PRINT("Command stack size: 4 KB per shell");

    PRINT("  #  name                 peak/size   canary");
    for (int i = 0; i < g_task_count; i++) {
        UaosTask *t = &g_tasks[i];
        if (t->tc_State == TASK_REMOVED || !t->native_stack_base) continue;
        uint32_t pk = Task_StackPeakUsed(t);

        cmd_scopy(line, " ", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)i, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
        int sl = cmd_slen(line); while (sl++ < 4) cmd_scat(line, " ", CMD_MAX_LINE);
        cmd_scat(line, t->ln_Name ? t->ln_Name : "?", CMD_MAX_LINE);
        sl = cmd_slen(line); while (sl++ < 25) cmd_scat(line, " ", CMD_MAX_LINE);
        cmd_uint_to_dec(pk, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, "/", CMD_MAX_LINE);
        cmd_uint_to_dec(t->native_stack_size, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
        sl = cmd_slen(line); while (sl++ < 38) cmd_scat(line, " ", CMD_MAX_LINE);
        cmd_scat(line, t->stack_overflowed ? "** DEAD **" : "ok", CMD_MAX_LINE);
        /* near-full warning at 90% */
        if (!t->stack_overflowed && pk * 10 >= t->native_stack_size * 9)
            cmd_scat(line, "  * near full *", CMD_MAX_LINE);
        PRINT(line);
    }
}
