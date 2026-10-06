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

static volatile uint64_t g_pit_last_tsc;
static volatile uint64_t g_pit_min = ~0ULL, g_pit_max, g_pit_sum, g_pit_n;
static volatile uint64_t g_pit_min_idx, g_pit_max_idx;   /* sample # of extremes */
static volatile uint64_t g_pit_min_prev;               /* delta before the min */
static volatile uint64_t g_pit_last_d;
static volatile uint8_t  g_pit_warm;    /* drop the first delta after seeding */

static volatile uint64_t g_irq_hist[TICKMON_LAT_BUCKETS];
static volatile uint64_t g_vec_max[256];

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
    if (d > g_pit_max) { g_pit_max = d; g_pit_max_idx = g_pit_n + 1; }
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
    if (cycles > g_vec_max[vector]) g_vec_max[vector] = cycles;
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
    for (int i = 0; i < TICKMON_LAT_BUCKETS; i++)
        out->irq_hist[i] = g_irq_hist[i];
    for (int i = 0; i < 256; i++)
        out->vec_max_cycles[i] = g_vec_max[i];
    irq_restore(fl);
}

void Tickmon_Clear(void)
{
    uint64_t fl = irq_save();
    g_pit_min = ~0ULL;
    g_pit_max = g_pit_sum = g_pit_n = 0;
    g_pit_min_idx = g_pit_max_idx = g_pit_min_prev = g_pit_last_d = 0;
    for (int i = 0; i < TICKMON_LAT_BUCKETS; i++) g_irq_hist[i] = 0;
    for (int i = 0; i < 256; i++) g_vec_max[i] = 0;
    irq_restore(fl);
}
