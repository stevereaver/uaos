/* prof.c — PIT-sampled RIP profiler (UAOS-210)
 *
 * Every PIT tick, samples the RIP of the interrupted code (the outermost
 * ISR frame stashed by ISR_Dispatch) plus the current task index, and
 * accumulates into a fixed open-addressed table.  ~100 Hz sampling gives
 * a cheap answer to "where does the CPU actually spend its time?"
 *
 *   C:prof start          begin sampling (clears previous run)
 *   C:prof stop           stop sampling
 *   C:prof                report top hotspots
 *   C:prof dump FILE=...  write "rip taskidx count" rows for
 *                         tools/prof_report.py (host-side symbolisation)
 */

#include "diag.h"
#include "../exec/task.h"
#include "../irq/idt.h"
#include "../dos/vfs.h"
#include "../boot/kprint.h"

#define PROF_SLOTS   4096

typedef struct {
    uint64_t rip;
    uint32_t count;
    uint32_t task_idx;   /* g_tasks index of last task seen at this RIP */
} ProfSlot;

static ProfSlot   g_slots[PROF_SLOTS];
static volatile int      g_prof_on;
static volatile uint32_t g_samples;
static volatile uint32_t g_collisions;

void Prof_Start(void)
{
    for (int i = 0; i < PROF_SLOTS; i++) { g_slots[i].rip = 0; g_slots[i].count = 0; g_slots[i].task_idx = 0; }
    g_samples = 0;
    g_collisions = 0;
    g_prof_on = 1;
}

void Prof_Stop(void) { g_prof_on = 0; }
int  Prof_Running(void) { return g_prof_on; }
uint32_t Prof_SampleCount(void) { return g_samples; }

void Prof_Tick(void)
{
    if (!g_prof_on) return;

    /* Sample the interrupted code's RIP.  If we're at outermost IRQ
     * depth the stashed frame is the PIT frame — the task's own code.
     * Nested-IRQ samples land on the inner handler's frame, still
     * truthful for "where did time go". */
    IsrFrame *f = IDT_LastIsrFrame();
    uint64_t rip = f ? f->rip : 0;
    if (!rip) return;

    UaosTask *cur = Task_Current();
    uint32_t tidx = cur ? (uint32_t)(cur - g_tasks) : 0xFFFF;

    uint32_t h = (uint32_t)((rip * 0x9E3779B97F4A7C15ULL) >> 32)
                 & (PROF_SLOTS - 1);
    for (int i = 0; i < 8; i++) {
        ProfSlot *s = &g_slots[(h + i) & (PROF_SLOTS - 1)];
        if (s->rip == rip) {
            s->count++;
            s->task_idx = tidx;
            g_samples++;
            return;
        }
        if (s->rip == 0) {
            s->rip = rip;
            s->count = 1;
            s->task_idx = tidx;
            g_samples++;
            return;
        }
    }
    g_collisions++;
    g_samples++;
}

void Prof_Report(void *ctx, DiagEmitFn emit, int top_n)
{
    DiagLine l;

    dl_reset(&l);
    dl_add(&l, "prof: "); dl_dec(&l, g_samples);
    dl_add(&l, " samples, "); dl_dec(&l, g_collisions);
    dl_add(&l, " dropped (table full)");
    dl_emit(&l, ctx, emit);

    if (!g_samples) return;

    /* Top-N by repeated max-extraction; `used` records printed slot
     * indices so equal counts don't produce duplicates. */
    int used[64];
    if (top_n > 64) top_n = 64;
    int printed = 0;
    for (int pass = 0; pass < top_n; pass++) {
        int best = -1;
        uint64_t bestc = 0;
        for (int i = 0; i < PROF_SLOTS; i++) {
            uint64_t c = g_slots[i].count;
            if (!g_slots[i].rip) continue;
            int dup = 0;
            for (int j = 0; j < printed; j++)
                if (used[j] == i) { dup = 1; break; }
            if (dup) continue;
            if (best < 0 || c > bestc) { best = i; bestc = c; }
        }
        if (best < 0 || bestc == 0) break;
        used[printed] = best;

        dl_reset(&l);
        dl_add(&l, " "); dl_hex(&l, g_slots[best].rip); dl_pad(&l, 20);
        dl_dec(&l, bestc); dl_pad(&l, 30);
        uint32_t pct = (uint32_t)((bestc * 100) / g_samples);
        dl_dec(&l, pct); dl_add(&l, "%");
        if (g_slots[best].task_idx != 0xFFFF &&
            g_slots[best].task_idx < MAX_TASKS) {
            UaosTask *t = &g_tasks[g_slots[best].task_idx];
            if (t->ln_Name) { dl_add(&l, "  "); dl_add(&l, t->ln_Name); }
        }
        dl_emit(&l, ctx, emit);
        printed++;
    }
    if (!printed) { dl_add(&l, " (no samples recorded)"); dl_emit(&l, ctx, emit); }
}

int Prof_DumpFile(const char *path)
{
    VfsFile f;
    if (!VFS_Open(&f, path, VFS_WRITE | VFS_CREATE | VFS_TRUNC))
        return 0;

    DiagLine l;
    for (int i = 0; i < PROF_SLOTS; i++) {
        if (!g_slots[i].rip) continue;
        dl_reset(&l);
        dl_hex(&l, g_slots[i].rip);
        dl_ch(&l, ' ');
        dl_dec(&l, g_slots[i].task_idx);
        dl_ch(&l, ' ');
        dl_dec(&l, g_slots[i].count);
        dl_ch(&l, '\n');
        VFS_Write(&f, (const uint8_t *)l.buf, (uint32_t)l.len);
    }
    VFS_Close(&f);
    return 1;
}
