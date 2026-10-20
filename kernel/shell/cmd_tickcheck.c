/* cmd_tickcheck.c — C:tickcheck — PIT timing + IRQ latency (UAOS-208)
 *
 *   tickcheck        PIT period stats vs calibrated TSC, IRQ dispatch
 *                    latency histogram, worst vectors, PIT ISR phase
 *                    profile, OUT0 dispatch phase, plus an RTC
 *                    cross-check of the tick rate (UAOS-332/334)
 *   tickcheck SEC=n  also measure TSC-vs-PIT drift over n seconds
 *   tickcheck RTC=n  RTC cross-check over n seconds instead of the
 *                    default 3 (n = 2..30)
 *   tickcheck CLEAR  re-baseline all tickmon counters
 */

#include "cmd_internal.h"
#include "../dbg/diag.h"
#include "../irq/idt.h"
#include "../irq/irq.h"
#include "../irq/pit.h"
#include "../irq/rtc.h"

extern volatile uint64_t g_pit_ticks;
extern void Timer_TickSubphases(uint64_t out[8], int clear);

static inline void es_outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t es_inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* i8254 channel-0 count latch: control word 0x00 = channel 0, latch. */
static uint16_t pit_ch0_latch(void)
{
    es_outb(0x43, 0x00);
    uint8_t lo = es_inb(0x40);
    uint8_t hi = es_inb(0x40);
    return (uint16_t)(lo | ((uint16_t)hi << 8));
}

/* RTC cross-check (UAOS-334).  Tickmon self-calibrates the TSC against
 * PIT ticks, so it can never detect that the ticks themselves arrive at
 * the wrong rate — this measures g_pit_ticks against IRQ8 update-ended
 * interrupts, which fire once per wall-clock second, and separately
 * counts channel-0 counter wraps so a doubled IRQ dispatch would show
 * pit_ticks/rtc-s != ch0_edges/rtc-s instead of a fast PIT.  Spun, not
 * yielded: this must still report when the PIT itself is dead. */
static void rtc_crosscheck(NativeCmdCtx *ctx, int secs)
{
    (void)ctx;
    char line[CMD_MAX_LINE], num[24];
    int rtc_vec = IRQ_VecForGsi(8);
    if (rtc_vec < 0) {
        PRINT("  rtc: IRQ8 not routed -- cross-check unavailable");
        return;
    }

    /* Wall-time cap: rdtsc is host-rate, so (secs+5) s at a worst-case
     * 6 GHz bounds the spin even if the RTC never fires. */
    uint64_t tsc_deadline = diag_rdtsc() + ((uint64_t)secs + 5) * 6000000000ULL;

    /* Sync to an IRQ8 edge, then count for `secs` further edges. */
    uint64_t r0 = IDT_VectorCount(rtc_vec);
    while (IDT_VectorCount(rtc_vec) == r0) {
        if (diag_rdtsc() > tsc_deadline) {
            PRINT("  rtc: no IRQ8 edges -- cross-check unavailable");
            return;
        }
        (void)pit_ch0_latch();
    }

    /* Re-base: the sync edge already advanced the count once. */
    r0 = IDT_VectorCount(rtc_vec);
    uint64_t tk0   = g_pit_ticks;
    uint64_t edges = 0;
    uint16_t cprev = pit_ch0_latch();
    int got = 0;
    while (got < secs) {
        if (diag_rdtsc() > tsc_deadline) break;
        uint16_t c = pit_ch0_latch();
        /* Mode-3 count decrements; a jump upward means the counter
         * reloaded — one full OUT0 period = one timer edge. */
        if (c > (uint16_t)(cprev + 8)) edges++;
        cprev = c;
        if (IDT_VectorCount(rtc_vec) - r0 > (uint64_t)got) got++;
    }
    uint64_t tk1 = g_pit_ticks;

    if (got < secs) {
        cmd_scopy(line, "  rtc: gave up (", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)got, num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, "/", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)secs, num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, " rtc s)", CMD_MAX_LINE);
        PRINT(line);
        return;
    }

    /* The mode-3 counter reloads TWICE per OUT0 period (once per half
     * cycle), so wrap count / 2 = completed periods — what an edge
     * detector on the irq line should count. */
    uint64_t periods = edges / 2;

    /* ticks*10/secs renders one decimal: 1000 -> "100.0". */
    cmd_scopy(line, "  rtc x-check: pit_ticks=", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)(tk1 - tk0), num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " ch0_reloads=", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)edges, num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " over ", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)secs, num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " rtc s  =>  pit=", CMD_MAX_LINE);
    uint64_t rate10 = (tk1 - tk0) * 10 / (uint64_t)secs;
    cmd_uint_to_dec((uint32_t)(rate10 / 10), num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, ".", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)(rate10 % 10), num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, "/s periods=", CMD_MAX_LINE);
    rate10 = periods * 10 / (uint64_t)secs;
    cmd_uint_to_dec((uint32_t)(rate10 / 10), num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, ".", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)(rate10 % 10), num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, "/s", CMD_MAX_LINE);
    PRINT(line);

    /* pit != UAOS_PIT_HZ while periods == UAOS_PIT_HZ => interrupts are
     * delivered at a multiple of the real OUT0 rate (double dispatch).
     * Both wrong together => the timer line itself is mis-running. */
    {
        uint64_t expect = (uint64_t)UAOS_PIT_HZ * (uint64_t)secs;
        uint64_t got    = tk1 - tk0;
        uint64_t diff   = (got > expect) ? got - expect : expect - got;
        if (diff * 50 > expect) {            /* >2% off nominal */
            cmd_scopy(line, "  WARNING: PIT tick rate off >2% vs RTC (expect ",
                      CMD_MAX_LINE);
            cmd_uint_to_dec((uint32_t)UAOS_PIT_HZ, num, sizeof(num));
            cmd_scat(line, num, CMD_MAX_LINE);
            cmd_scat(line, "/s)", CMD_MAX_LINE);
            PRINT(line);
        }
    }
}

static void hexn(char *line, uint64_t v, int digits)
{
    char h[17];
    for (int i = digits - 1; i >= 0; i--) { h[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
    h[digits] = 0;
    cmd_scat(line, h, CMD_MAX_LINE);
}

static const char *const k_phase_names[TICKMON_PIT_PHASES] = {
    "tickmon", "prof", "storm", "net", "kbd",
    "timer", "waketmr", "watchdog", "cpufreq", "sched",
};

/* Print a tickmon-stored cycle count in kcycles. */
static void kcyc(char *line, uint64_t cyc)
{
    char n[24];
    cmd_uint_to_dec((uint32_t)(cyc / 1000), n, sizeof(n));
    cmd_scat(line, n, CMD_MAX_LINE);
    cmd_scat(line, "k", CMD_MAX_LINE);
}

void Cmd_Tickcheck(NativeCmdCtx *ctx, const char *args)
{
    char line[CMD_MAX_LINE], num[24];
    uint64_t hz = Tickmon_TscHz();

    if (cmd_kw_find(args, "clear")) {
        uint64_t tt[8];
        Tickmon_Clear();
        Timer_TickSubphases(tt, 1);
        PRINT("tickcheck: counters cleared");
        return;
    }

    const char *rtcp = cmd_kv_find(args, "rtc");
    if (rtcp) {
        uint64_t rs;
        if (cmd_parse_uint(rtcp, &rs) && rs >= 2 && rs <= 30) {
            rtc_crosscheck(ctx, (int)rs);
        } else {
            PRINT("usage: tickcheck [SEC=1..60] [RTC=2..30]");
        }
        return;
    }

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

        /* TSC window of the worst period — correlates with etrace/klog. */
        if (s.pit_max_t1) {
            cmd_scopy(line, "  max window: t=", CMD_MAX_LINE);
            cmd_uint_to_dec((uint32_t)(s.pit_max_idx / UAOS_PIT_HZ),
                            num, sizeof(num));
            cmd_scat(line, num, CMD_MAX_LINE);
            cmd_scat(line, "s  tsc 0x", CMD_MAX_LINE);
            hexn(line, s.pit_max_t0, 12);
            cmd_scat(line, " -> 0x", CMD_MAX_LINE);
            hexn(line, s.pit_max_t1, 12);
            PRINT(line);
        }
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
        cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, "k @t=", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)(s.vec_max_at[v] / UAOS_PIT_HZ),
                        num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, "s  ", CMD_MAX_LINE);
        if (cmd_slen(line) > 60) { PRINT(line); cmd_scopy(line, "  ", CMD_MAX_LINE); }
    }
    if (any) PRINT(line); else PRINT("  (no samples)");

    /* UAOS-332: which PIT_IRQHandler call site owns the worst dispatch. */
    PRINT("pit isr phases (max cyc / at):");
    any = 0;
    cmd_scopy(line, "  ", CMD_MAX_LINE);
    for (int i = 0; i < TICKMON_PIT_PHASES; i++) {
        if (!s.phase_max[i]) continue;
        any = 1;
        cmd_scat(line, k_phase_names[i], CMD_MAX_LINE);
        cmd_scat(line, "=", CMD_MAX_LINE);
        kcyc(line, s.phase_max[i]);
        cmd_scat(line, "@t=", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)(s.phase_max_at[i] / UAOS_PIT_HZ),
                        num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, "s  ", CMD_MAX_LINE);
        if (cmd_slen(line) > 60) { PRINT(line); cmd_scopy(line, "  ", CMD_MAX_LINE); }
    }
    if (any) PRINT(line); else PRINT("  (no samples)");

    /* UAOS-335: sub-phase maxima inside timer_ProcessTicks — which tick
     * body owns the worst "timer" phase above. */
    {
        static const char *const k_tt_names[8] = {
            "beam", "cia", "flp", "aud", "kbd", "usb", "ser", "vbl",
        };
        uint64_t tt[8];
        Timer_TickSubphases(tt, 0);
        PRINT("pit timer sub-phases (max cyc):");
        any = 0;
        cmd_scopy(line, "  ", CMD_MAX_LINE);
        for (int i = 0; i < 8; i++) {
            if (!tt[i]) continue;
            any = 1;
            cmd_scat(line, k_tt_names[i], CMD_MAX_LINE);
            cmd_scat(line, "=", CMD_MAX_LINE);
            kcyc(line, tt[i]);
            cmd_scat(line, "  ", CMD_MAX_LINE);
        }
        if (any) PRINT(line); else PRINT("  (no samples)");
    }

    if (s.whole_max) {
        cmd_scopy(line, "worst pit dispatch ", CMD_MAX_LINE);
        kcyc(line, s.whole_max);
        cmd_scat(line, " @t=", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)(s.whole_max_at / UAOS_PIT_HZ),
                        num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, "s:", CMD_MAX_LINE);
        PRINT(line);
        cmd_scopy(line, "  ", CMD_MAX_LINE);
        for (int i = 0; i < TICKMON_PIT_PHASES; i++) {
            cmd_scat(line, k_phase_names[i], CMD_MAX_LINE);
            cmd_scat(line, "=", CMD_MAX_LINE);
            kcyc(line, s.worst_phases[i]);
            cmd_scat(line, "  ", CMD_MAX_LINE);
            if (cmd_slen(line) > 60) {
                PRINT(line); cmd_scopy(line, "  ", CMD_MAX_LINE);
            }
        }
        PRINT(line);
    }

    /* UAOS-334: adaptive OUT0 gate state — "calibrating" during the
     * first 256 dispatches, "on" when double-edge delivery was
     * confirmed (or pitgate=on), "off" on single-edge hardware (or
     * pitgate=off; read-back is then shut off too). */
    cmd_scopy(line, "pit out0 gate: ", CMD_MAX_LINE);
    cmd_scat(line, s.pit_gstate == 2 ? "off" :
                   s.pit_gstate == 1 ? "on"  : "calibrating",
             CMD_MAX_LINE);
    PRINT(line);

    /* Which OUT0 phase each PIT dispatch landed in.  The i8254
     * runs mode 3 (square wave) — out=1 for the first half of each
     * period, out=0 for the second.  Dispatches in both states mean the
     * CPU sees both transitions (delivery doubling); all-one-state is
     * the normal one-interrupt-per-period signature. */
    if (s.phase_idx) {
        cmd_scopy(line, "pit out0 phase: out=1:", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)s.phase_out1, num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, " out=0:", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)s.phase_out0, num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, " alt:", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)s.phase_alt, num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
        PRINT(line);
        cmd_scopy(line, "  last: ", CMD_MAX_LINE);
        int shown = 0;
        for (int i = (int)s.phase_idx - TICKMON_PHASE_RING;
             i < (int)s.phase_idx; i++) {
            if (i < 0) continue;
            uint8_t st = s.phase_status[i % TICKMON_PHASE_RING];
            cmd_scat(line, (st & 0x80) ? "1:" : "0:", CMD_MAX_LINE);
            cmd_uint_to_dec(s.phase_count[i % TICKMON_PHASE_RING],
                            num, sizeof(num));
            cmd_scat(line, num, CMD_MAX_LINE);
            cmd_scat(line, "  ", CMD_MAX_LINE);
            if (++shown == 8) break;
        }
        PRINT(line);
    }

    /* Independent rate check — TSC self-calibration cannot see a wrong
     * PIT frequency; IRQ8 update-ended ticks are wall-clock seconds. */
    rtc_crosscheck(ctx, 3);
}
