/* cmd_memcheck.c — C:memcheck — Mungwall-style heap debugging control
 *
 *   memcheck          status + scan (live allocs, violations)
 *   memcheck on       enable AllocMem guards + free-list poisoning
 *   memcheck off      disable
 *   memcheck test     self-test: deliberate tail overwrite → expect a hit
 *
 * While enabled, every exec.library AllocMem gets 4-byte guard words
 * front+tail, freed blocks are poison-filled, and FreeMem validates
 * signatures — violations are reported via klog [memchk] with the
 * allocating and freeing task names.
 */

#include "cmd_internal.h"
#include "../exec/memcheck.h"

static NativeCmdCtx *g_mc_ctx;

static void mc_dump_emit(const char *line)
{
    CMD_PRINT(g_mc_ctx, line);
}

void Cmd_Memcheck(NativeCmdCtx *ctx, const char *args)
{
    char line[CMD_MAX_LINE], num[24];

    if (args && *args) {
        if (cmd_seq_ci(args, "dump")) {
            g_mc_ctx = ctx;
            PRINT("chip free list:");
            Memcheck_DumpList(0, mc_dump_emit);
            PRINT("fast free list:");
            Memcheck_DumpList(1, mc_dump_emit);
            return;
        }
        if (cmd_seq_ci(args, "on")) {
            Memcheck_SetEnabled(1);
            PRINT("memcheck: ON — AllocMem guard bands + free poison active");
            return;
        }
        if (cmd_seq_ci(args, "off")) {
            Memcheck_SetEnabled(0);
            PRINT("memcheck: OFF");
            return;
        }
        if (cmd_seq_ci(args, "test")) {
            uint32_t bad = Memcheck_SelfTest();
            cmd_scopy(line, "memcheck test: ", CMD_MAX_LINE);
            cmd_uint_to_dec(bad, num, sizeof(num));
            cmd_scat(line, num, CMD_MAX_LINE);
            cmd_scat(line, bad ? " violation(s) reported — see dmesg exec"
                              : " violations (unexpected!)",
                     CMD_MAX_LINE);
            PRINT(line);
            return;
        }
        PRINT("usage: memcheck [on|off|test]");
        return;
    }

    cmd_scopy(line, "memcheck: ", CMD_MAX_LINE);
    cmd_scat(line, Memcheck_IsEnabled() ? "ON" : "OFF", CMD_MAX_LINE);
    cmd_scat(line, ", ", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)Memcheck_LiveCount(), num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " live tracked allocs", CMD_MAX_LINE);
    PRINT(line);

    uint32_t bad = Memcheck_Scan();
    cmd_scopy(line, "scan: ", CMD_MAX_LINE);
    cmd_uint_to_dec(bad, num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, bad ? " violation(s) — see dmesg exec for details"
                       : " violations",
             CMD_MAX_LINE);
    PRINT(line);
}
