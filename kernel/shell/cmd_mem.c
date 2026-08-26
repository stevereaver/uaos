/* cmd_mem.c — C:mem — display memory information
 *
 * Queries the kernel-exported memory API (Mem_GetInfo) and prints a
 * point-in-time snapshot of the live memory arenas instead of the
 * previously hardcoded placeholder values.
 */

#include "cmd_internal.h"
#include "../exec/mem_info.h"

static void mem_fmt(uint32_t bytes, char *buf, int max)
{
    if (bytes >= (1024u * 1024u)) {
        cmd_uint_to_dec(bytes / (1024u * 1024u), buf, max);
        cmd_scat(buf, " MB", max);
    } else {
        cmd_uint_to_dec(bytes / 1024u, buf, max);
        cmd_scat(buf, " KB", max);
    }
}

void Cmd_Mem(NativeCmdCtx *ctx, const char *args)
{
    (void)args;

    struct UaosMemInfo m;
    Mem_GetInfo(&m);

    char line[CMD_MAX_LINE];
    char num[24];

    /* x86-64 userspace heap */
    cmd_scopy(line, "X64 heap:  ", CMD_MAX_LINE);
    mem_fmt(m.x64_used, num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " / ", CMD_MAX_LINE);
    mem_fmt(m.x64_total, num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " used", CMD_MAX_LINE);
    PRINT(line);

    cmd_scopy(line, "           ", CMD_MAX_LINE);
    mem_fmt(m.x64_free, num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " free", CMD_MAX_LINE);
    PRINT(line);

    /* Emulated M68k guest RAM slots */
    cmd_scopy(line, "M68k RAM:  ", CMD_MAX_LINE);
    cmd_uint_to_dec(m.m68k_slots_used, num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " / ", CMD_MAX_LINE);
    cmd_uint_to_dec(m.m68k_slots_total, num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " slots, ", CMD_MAX_LINE);
    mem_fmt(m.m68k_ram_total, num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, "/slot", CMD_MAX_LINE);
    PRINT(line);

    /* Scheduler task summary */
    cmd_scopy(line, "Tasks:     ", CMD_MAX_LINE);
    cmd_uint_to_dec(m.tasks_total, num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " total, ", CMD_MAX_LINE);
    cmd_uint_to_dec(m.tasks_running, num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " running, ", CMD_MAX_LINE);
    cmd_uint_to_dec(m.tasks_waiting, num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " waiting", CMD_MAX_LINE);
    PRINT(line);
}
