/* tickmon.c — PIT tick jitter + IRQ dispatch latency instrumentation
 * (UAOS-208)
 *
 * Two cheap measurements, both rdtsc-based, collected unconditionally so
 * C:tickcheck has live data the moment it runs:
 *
 *  - Tickmon_PitTick(): called every PIT_IRQHandler with the TSC; the
 *    deltas between successive calls are the tick delivery jitter —
 *    catches early/late/lost ticks and IRQ-starvation noise.
 *  - Tickmon_IrqDur(): called by ISR_Dispatch around every registered
 *    handler body; durations land in a log2(cycles) histogram and a
 *    per-vector worst-case table.
 *
 * Cycles are stored raw — converting to microseconds needs a calibrated
 * TSC frequency, which tickcheck measures itself at report time.
 */

#include "diag.h"
#include "../irq/irq.h"     /* irq_save/irq_restore */
#include "../boot/kprint.h" /* gate decision log lines */

static volatile uint64_t g_pit_last_tsc;
static volatile uint64_t g_pit_min = ~0ULL, g_pit_max, g_pit_sum, g_pit_n;
static volatile uint64_t g_pit_min_idx, g_pit_max_idx;   /* sample # of extremes */
static volatile uint64_t g_pit_min_prev;               /* delta before the min */
static volatile uint64_t g_pit_last_d;
static volatile uint8_t  g_pit_warm;    /* drop the first delta after seeding */

static volatile uint64_t g_irq_hist[TICKMON_LAT_BUCKETS];
static volatile uint64_t g_vec_max[256];
static volatile uint64_t g_vec_max_at[256];  /* g_pit_ticks at each worst */
static volatile uint64_t g_pit_max_t0, g_pit_max_t1; /* TSC window of max period */

/* UAOS-334: PIT channel-0 status/count latched at every dispatch.
 * The i8254 is programmed mode 3 (square wave): OUT0 is high for the
 * first half of each period and low for the second.  One dispatch per
 * OUT0 period must always land in the same phase — samples in both
 * OUT states mean both transitions are reaching the CPU. */
extern volatile uint64_t g_pit_ticks;
static volatile uint8_t  g_ph_status[TICKMON_PHASE_RING];
static volatile uint16_t g_ph_count[TICKMON_PHASE_RING];
static volatile uint32_t g_ph_idx;
static volatile uint64_t g_ph_out1, g_ph_out0, g_ph_alt;
static volatile uint8_t  g_ph_prev_out, g_ph_seen;

/* Adaptive OUT0 gate (UAOS-334 rework): QEMU's i8254 raises the IRQ0
 * level callback on every mode-3 OUT transition, so its edge path
 * delivers on both phases; real hardware only delivers OUT=0->1.  An
 * unconditional gate would drop a legitimate tick when ISR latency
 * crosses the half-period boundary, so calibrate instead: gate is ON
 * with read-back for the first TICKMON_CAL_N dispatches; if the OUT=0
 * share is high the delivery is doubled and the gate stays, if it is
 * near zero the box is single-edge and both gate and read-back shut
 * off (zero per-tick port I/O on metal). */
volatile uint8_t g_pit_rb   = 1;   /* read-back active   */
volatile uint8_t g_pit_gate = 1;   /* drop OUT=0 entries */
volatile uint8_t g_pit_gstate;     /* 0 calib 1 on 2 off */
static volatile uint16_t g_cal_n, g_cal_out0;
#define TICKMON_CAL_N 256

void Tickmon_PitGateForce(int mode)          /* boot arg pitgate= */
{
    if (mode == 2) { g_pit_rb = 0; g_pit_gate = 0; g_pit_gstate = 2; }
    else           { g_pit_rb = 1; g_pit_gate = 1; g_pit_gstate = 1; }
}

void Tickmon_PitPhase(uint8_t status, uint16_t count)
{
    uint8_t out = status >> 7;
    uint32_t i = g_ph_idx++ % TICKMON_PHASE_RING;
    g_ph_status[i] = status;
    g_ph_count[i]  = count;
    if (g_ph_seen && out != g_ph_prev_out) g_ph_alt++;
    if (out) g_ph_out1++; else g_ph_out0++;
    g_ph_prev_out = out;
    g_ph_seen = 1;

    if (!g_pit_gstate) {
        g_cal_n++;
        if (!out) g_cal_out0++;
        if (g_cal_n >= TICKMON_CAL_N) {
            uint32_t pct = (uint32_t)g_cal_out0 * 100u / g_cal_n;
            if (pct < 10) {
                g_pit_gate   = 0;
                g_pit_rb     = 0;
                g_pit_gstate = 2;
                kprint("[PIT] single-edge IRQ0 delivery -- OUT0 gate off\n");
            } else {
                g_pit_gstate = 1;
                if (pct >= 25) {
                    kprint("[PIT] double-edge IRQ0 delivery detected "
                           "-- OUT0 gate on\n");
                } else {
                    kprint("[PIT] mixed OUT0 phases (");
                    kprintdec(pct);
                    kprint("% low) -- OUT0 gate on\n");
                }
            }
        }
    }
}

/* UAOS-332: per-call-site cycle deltas inside PIT_IRQHandler. */
static volatile uint64_t g_phase_max[TICKMON_PIT_PHASES];
static volatile uint64_t g_phase_max_at[TICKMON_PIT_PHASES];
static volatile uint64_t g_whole_max;
static volatile uint64_t g_whole_max_at;
static volatile uint64_t g_worst_phase[TICKMON_PIT_PHASES];

void Tickmon_PitPhases(const uint64_t *deltas, int n)
{
    uint64_t whole = 0;
    if (n > TICKMON_PIT_PHASES) n = TICKMON_PIT_PHASES;
    for (int i = 0; i < n; i++) {
        whole += deltas[i];
        if (deltas[i] > g_phase_max[i]) {
            g_phase_max[i]    = deltas[i];
            g_phase_max_at[i] = g_pit_ticks;
        }
    }
    if (whole > g_whole_max) {
        g_whole_max    = whole;
        g_whole_max_at = g_pit_ticks;
        for (int i = 0; i < TICKMON_PIT_PHASES; i++)
            g_worst_phase[i] = (i < n) ? deltas[i] : 0;
    }
}

/* Running TSC-frequency estimate in Hz — refreshed every PIT tick from
 * the cumulative delta, so it's self-calibrating with zero setup.
 * 0 until the second tick arrives. */
static volatile uint64_t g_tsc_hz;

void Tickmon_PitTick(uint64_t tsc)
{
    uint64_t prev = g_pit_last_tsc;
    g_pit_last_tsc = tsc;
    if (!prev) { g_pit_warm = 1; return; }

    /* The delta after the seed is not a period: the seeding dispatch can
     * be a PIT edge latched while IF=0 during early init, delivered at
     * an arbitrary phase of the 10 ms cycle when interrupts first open
     * (UAOS-270).  Drop it — it pins min with a bogus sub-period sample. */
    if (g_pit_warm) { g_pit_warm = 0; return; }

    uint64_t d = tsc - prev;
    if (d < g_pit_min) {
        g_pit_min      = d;
        g_pit_min_idx  = g_pit_n + 1;
        g_pit_min_prev = g_pit_last_d;   /* long prev => delayed-tick pair */
    }
    if (d > g_pit_max) {
        g_pit_max     = d;
        g_pit_max_idx = g_pit_n + 1;
        /* TSC window of the longest period — lets etrace/klog events be
         * correlated against the stall. */
        g_pit_max_t0  = prev;
        g_pit_max_t1  = tsc;
    }
    g_pit_last_d = d;
    g_pit_sum += d;
    g_pit_n++;
    if (g_pit_n >= 100)
        g_tsc_hz = (g_pit_sum / g_pit_n) * 100;   /* 100 ticks per second */
}

void Tickmon_IrqDur(uint8_t vector, uint64_t cycles)
{
    /* log2 bucket: 0-1 cycles -> 0, 2-3 -> 1, ... keeps the histogram
     * small while separating fast tick handlers from DMA-setup hangs. */
    int b = 0;
    uint64_t c = cycles;
    while (c >>= 1) b++;
    if (b >= TICKMON_LAT_BUCKETS) b = TICKMON_LAT_BUCKETS - 1;
    g_irq_hist[b]++;
    if (cycles > g_vec_max[vector]) {
        g_vec_max[vector]    = cycles;
        g_vec_max_at[vector] = g_pit_ticks;
    }
}

uint64_t Tickmon_TscHz(void) { return g_tsc_hz; }

void Tickmon_Snapshot(TickmonStats *out)
{
    uint64_t fl = irq_save();
    out->pit_last_tsc  = g_pit_last_tsc;
    out->pit_min_delta = (g_pit_min == ~0ULL) ? 0 : g_pit_min;
    out->pit_max_delta = g_pit_max;
    out->pit_sum_delta = g_pit_sum;
    out->pit_samples   = g_pit_n;
    out->pit_min_idx   = g_pit_min_idx;
    out->pit_max_idx   = g_pit_max_idx;
    out->pit_min_prev  = g_pit_min_prev;
    out->pit_max_t0    = g_pit_max_t0;
    out->pit_max_t1    = g_pit_max_t1;
    for (int i = 0; i < TICKMON_LAT_BUCKETS; i++)
        out->irq_hist[i] = g_irq_hist[i];
    for (int i = 0; i < 256; i++) {
        out->vec_max_cycles[i] = g_vec_max[i];
        out->vec_max_at[i]     = g_vec_max_at[i];
    }
    out->phase_out1 = g_ph_out1;
    out->phase_out0 = g_ph_out0;
    out->phase_alt  = g_ph_alt;
    out->phase_idx  = g_ph_idx;
    for (int i = 0; i < TICKMON_PHASE_RING; i++) {
        out->phase_status[i] = g_ph_status[i];
        out->phase_count[i]  = g_ph_count[i];
    }
    for (int i = 0; i < TICKMON_PIT_PHASES; i++) {
        out->phase_max[i]      = g_phase_max[i];
        out->phase_max_at[i]   = g_phase_max_at[i];
        out->worst_phases[i]   = g_worst_phase[i];
    }
    out->whole_max    = g_whole_max;
    out->whole_max_at = g_whole_max_at;
    out->pit_gstate   = g_pit_gstate;
    irq_restore(fl);
}

void Tickmon_Clear(void)
{
    uint64_t fl = irq_save();
    g_pit_min = ~0ULL;
    g_pit_max = g_pit_sum = g_pit_n = 0;
    g_pit_min_idx = g_pit_max_idx = g_pit_min_prev = g_pit_last_d = 0;
    g_pit_max_t0 = g_pit_max_t1 = 0;
    for (int i = 0; i < TICKMON_LAT_BUCKETS; i++) g_irq_hist[i] = 0;
    for (int i = 0; i < 256; i++) { g_vec_max[i] = 0; g_vec_max_at[i] = 0; }
    g_ph_idx = g_ph_out1 = g_ph_out0 = g_ph_alt = g_ph_seen = 0;
    for (int i = 0; i < TICKMON_PIT_PHASES; i++) {
        g_phase_max[i] = g_phase_max_at[i] = g_worst_phase[i] = 0;
    }
    g_whole_max = g_whole_max_at = 0;
    irq_restore(fl);
}
