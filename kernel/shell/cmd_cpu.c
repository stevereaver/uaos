/* cmd_cpu.c — C:cpu — CPU frequency/power introspection (UAOS-272)
 *
 *   cpu       print EIST/C1E status, P-state table, current state,
 *             live busy% and transition count
 *   cpu ALL   also print the raw PERF_STATUS word
 */

#include "cmd_internal.h"
#include "../drivers/cpufreq.h"

static void u32d(char *line, uint32_t v)
{
    char num[16];
    cmd_uint_to_dec(v, num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
}

static void u32h(char *line, uint32_t v)
{
    char num[12]; int i = 0;
    num[i++] = '0'; num[i++] = 'x';
    for (int s = 28; s >= 0; s -= 4) {
        int nib = (int)((v >> s) & 0xF);
        num[i++] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    num[i] = 0;
    cmd_scat(line, num, CMD_MAX_LINE);
}

void Cmd_Cpu(NativeCmdCtx *ctx, const char *args)
{
    CpuFreqInfo cf;
    CpuFreq_GetInfo(&cf);
    char line[CMD_MAX_LINE];

    cmd_scopy(line, "cpu:  family ", CMD_MAX_LINE);
    u32d(line, (uint32_t)cf.cpu_family);
    cmd_scat(line, " model 0x", CMD_MAX_LINE);
    {   char m[4];
        int lo = cf.cpu_model & 0xF, hi = (cf.cpu_model >> 4) & 0xF;
        m[0] = (char)(hi < 10 ? '0' + hi : 'a' + hi - 10);
        m[1] = (char)(lo < 10 ? '0' + lo : 'a' + lo - 10);
        m[2] = 0;
        cmd_scat(line, m, CMD_MAX_LINE);
    }
    PRINT(line);

    cmd_scopy(line, "eist: ", CMD_MAX_LINE);
    if (cf.eist_on) {
        cmd_scat(line, cf.locked ? "enabled (fw-locked)" : "enabled", CMD_MAX_LINE);
    } else if (cf.est) {
        cmd_scat(line, cf.locked ? "present, locked off" :
                                   "present but disabled", CMD_MAX_LINE);
    } else {
        cmd_scat(line, "absent", CMD_MAX_LINE);
    }
    cmd_scat(line, "   c1e: ", CMD_MAX_LINE);
    cmd_scat(line, "n/a", CMD_MAX_LINE);   /* MSR_POWER_CTL absent on Core 2 */
    cmd_scat(line, "   tm1: ", CMD_MAX_LINE);
    cmd_scat(line, cf.tm1 ? "fallback active" : "-", CMD_MAX_LINE);
    cmd_scat(line, "   invtsc: ", CMD_MAX_LINE);
    cmd_scat(line, cf.inv_tsc ? "yes" : "no", CMD_MAX_LINE);
    PRINT(line);

    cmd_scopy(line, "acpi: ", CMD_MAX_LINE);
    u32d(line, cf.tables_scanned);
    cmd_scat(line, " table(s) scanned, ", CMD_MAX_LINE);
    u32d(line, cf.pss_hits);
    cmd_scat(line, " _PSS decl(s) seen", CMD_MAX_LINE);
    PRINT(line);

    if (cf.n_states > 0) {
        cmd_scopy(line, "pstates: ", CMD_MAX_LINE);
        cmd_scat(line, cf.table_source == 1 ? "acpi _PSS" :
                       cf.table_source == 2 ? "synthesised" :
                       cf.table_source == 3 ? "acpi (heuristic)" :
                       "unknown", CMD_MAX_LINE);
        cmd_scat(line, " (", CMD_MAX_LINE);
        u32d(line, (uint32_t)cf.n_states);
        cmd_scat(line, ")", CMD_MAX_LINE);
        PRINT(line);
        for (int i = 0; i < cf.n_states; i++) {
            cmd_scopy(line, "  [", CMD_MAX_LINE);
            u32d(line, (uint32_t)i);
            cmd_scat(line, "] ", CMD_MAX_LINE);
            u32d(line, cf.state_mhz[i]);
            cmd_scat(line, " MHz  ctl ", CMD_MAX_LINE);
            u32h(line, cf.state_ctl[i]);
            cmd_scat(line, "  sts ", CMD_MAX_LINE);
            u32h(line, cf.state_sts[i]);
            cmd_scat(line, i == cf.cur_idx ? "   <== current" : "", CMD_MAX_LINE);
            PRINT(line);
        }
    } else {
        PRINT("pstates: none");
    }

    cmd_scopy(line, "busy: ", CMD_MAX_LINE);
    u32d(line, cf.busy_pct);
    cmd_scat(line, "%   transitions: ", CMD_MAX_LINE);
    u32d(line, cf.transitions);
    PRINT(line);

    if (args && *args) {
        cmd_scopy(line, "perf_status[15:0]: ", CMD_MAX_LINE);
        u32h(line, cf.perf_status);
        cmd_scat(line, "   boot_ctl: ", CMD_MAX_LINE);
        u32h(line, cf.boot_ctl);
        PRINT(line);

        for (uint32_t i = 0; i < cf.dbg_tbln; i++) {
            cmd_scopy(line, "tbl@", CMD_MAX_LINE);
            u32h(line, cf.dbg_tbl[i]);
            cmd_scat(line, " len=", CMD_MAX_LINE);
            u32d(line, cf.dbg_tlen[i]);
            PRINT(line);
        }

        /* DBG: hex-dump 128 bytes starting 16 before each _PSS hit so
         * the AML encoding context can be read off telnet. */
        for (uint32_t i = 0; i < cf.dbg_n; i++) {
            const uint8_t *b = (const uint8_t *)(uintptr_t)
                (cf.dbg_addr[i] - 16);
            cmd_scopy(line, "hit@", CMD_MAX_LINE);
            u32h(line, cf.dbg_addr[i]);
            cmd_scat(line, ":", CMD_MAX_LINE);
            PRINT(line);
            for (int row = 0; row < 8; row++) {
                line[0] = 0;
                for (int k = 0; k < 16; k++) {
                    int v = b[row * 16 + k];
                    static const char hx[] = "0123456789abcdef";
                    int dl = cmd_slen(line);
                    line[dl] = hx[v >> 4];
                    line[dl + 1] = hx[v & 15];
                    line[dl + 2] = ' ';
                    line[dl + 3] = 0;
                }
                PRINT(line);
            }
        }
    }
}
