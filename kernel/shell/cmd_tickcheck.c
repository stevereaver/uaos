/* cmd_tickcheck.c — C:tickcheck — PIT timing + IRQ latency (UAOS-208)
 *
 *   tickcheck        PIT period stats vs calibrated TSC, IRQ dispatch
 *                    latency histogram, worst vectors
 *   tickcheck SEC=n  also measure TSC-vs-PIT drift over n seconds
 */

#include "cmd_internal.h"
#include "../dbg/diag.h"
#include "../irq/idt.h"

extern volatile uint64_t g_pit_ticks;

static void hexn(char *line, uint64_t v, int digits)
{
    char h[17];
    for (int i = digits - 1; i >= 0; i--) { h[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
    h[digits] = 0;
    cmd_scat(line, h, CMD_MAX_LINE);
}

void Cmd_Tickcheck(NativeCmdCtx *ctx, const char *args)
{
    char line[CMD_MAX_LINE], num[24];
    uint64_t hz = Tickmon_TscHz();

    const char *p = cmd_kv_find(args, "sec");
    if (p) {
        uint64_t secs;
        if (cmd_parse_uint(p, &secs) && secs >= 1 && secs <= 60) {
            TickmonStats a, b;
            Tickmon_Snapshot(&a);
            uint64_t ticks0 = g_pit_ticks;
            PRINT("tickcheck: sampling ...");
            for (uint64_t i = 0; i < secs * 10; i++) CMD_YIELD(ctx, 100);
            Tickmon_Snapshot(&b);
            uint64_t pticks = g_pit_ticks - ticks0;
            uint64_t dtsc = b.pit_last_tsc - a.pit_last_tsc;

            cmd_scopy(line, "drift over ", CMD_MAX_LINE);
            cmd_uint_to_dec((uint32_t)secs, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
            cmd_scat(line, " s: pit_ticks=", CMD_MAX_LINE);
            cmd_uint_to_dec((uint32_t)pticks, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
            cmd_scat(line, " tsc=", CMD_MAX_LINE);
            cmd_uint_to_dec((uint32_t)(dtsc / 1000000), num, sizeof(num));
            cmd_scat(line, num, CMD_MAX_LINE);
            cmd_scat(line, " Mcyc", CMD_MAX_LINE);
            PRINT(line);
            if (hz && pticks) {
                /* pit runs at 100 Hz: expected_tsc = pticks * hz / 100 */
                uint64_t exp = pticks * hz / 100;
                int64_t drift = (int64_t)dtsc - (int64_t)exp;
                int64_t ppm = exp ? drift * 1000000 / (int64_t)exp : 0;
                cmd_scopy(line, "  tsc/pit drift ", CMD_MAX_LINE);
                cmd_sdec((int32_t)ppm, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
                cmd_scat(line, " ppm", CMD_MAX_LINE);
                PRINT(line);
            }
        } else {
            PRINT("usage: tickcheck [SEC=1..60]");
            return;
        }
    }

    TickmonStats s;
    Tickmon_Snapshot(&s);

    cmd_scopy(line, "tsc_hz=", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)(hz / 1000), num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " kHz (self-cal)  pit_ticks=", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)g_pit_ticks, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
    PRINT(line);

    if (s.pit_samples) {
        uint64_t avg = s.pit_sum_delta / s.pit_samples;
        uint64_t exp = hz / 100;   /* ideal 10 ms period in cycles */
        cmd_scopy(line, "pit period (cyc): min=", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)(s.pit_min_delta / 1000), num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE); cmd_scat(line, "k avg=", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)(avg / 1000), num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE); cmd_scat(line, "k max=", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)(s.pit_max_delta / 1000), num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE); cmd_scat(line, "k  expect=", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)(exp / 1000), num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE); cmd_scat(line, "k", CMD_MAX_LINE);
        PRINT(line);

        /* Where the extremes landed — a short delta preceded by a long
         * one is a delayed-tick complement; a lone short delta is a
         * spurious dispatch. */
        cmd_scopy(line, "  min@sample ", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)s.pit_min_idx, num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, " (prev=", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)(s.pit_min_prev / 1000), num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, "k)  max@sample ", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)s.pit_max_idx, num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
        PRINT(line);
    }

    PRINT("irq dispatch latency (log2 cycles):");
    cmd_scopy(line, "  ", CMD_MAX_LINE);
    int any = 0;
    for (int i = 0; i < TICKMON_LAT_BUCKETS; i++) {
        if (!s.irq_hist[i]) continue;
        any = 1;
        cmd_uint_to_dec((uint32_t)i, num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE); cmd_scat(line, ":", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)s.irq_hist[i], num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE); cmd_scat(line, "  ", CMD_MAX_LINE);
        if (cmd_slen(line) > 70) { PRINT(line); cmd_scopy(line, "  ", CMD_MAX_LINE); }
    }
    if (any) PRINT(line); else PRINT("  (no samples)");

    PRINT("worst single dispatch per vector (cycles):");
    any = 0;
    cmd_scopy(line, "  ", CMD_MAX_LINE);
    for (int v = 0; v < 256; v++) {
        if (!s.vec_max_cycles[v]) continue;
        any = 1;
        cmd_scat(line, "v", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)v, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
        const char *nm = IDT_VectorName((uint8_t)v);
        if (nm) { cmd_scat(line, "(", CMD_MAX_LINE); cmd_scat(line, nm, CMD_MAX_LINE); cmd_scat(line, ")", CMD_MAX_LINE); }
        cmd_scat(line, "=", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)(s.vec_max_cycles[v] / 1000), num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE); cmd_scat(line, "k  ", CMD_MAX_LINE);
        if (cmd_slen(line) > 60) { PRINT(line); cmd_scopy(line, "  ", CMD_MAX_LINE); }
    }
    if (any) PRINT(line); else PRINT("  (no samples)");
}
