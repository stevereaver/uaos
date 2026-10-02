/* etrace.c — binary kernel event trace ring (UAOS-209)
 *
 * ftrace-lite: a fixed-size memory ring of compact binary records written
 * from hot paths (ISR dispatch, context switch, Signal, DoPkt, netdev
 * TX/RX).  Unlike klog it adds zero UART pressure — records are 16-byte
 * stores, not formatted strings — so sched/IRQ-rate events can be traced
 * on production builds without starving the serial line (the failure
 * that killed "klog all=trace" sessions).
 *
 * Records: (tsc, event_id, arg0, arg1).  Class masking via
 * Etrace_SetMask() — a single global test at each emit site, ~2 cycles
 * when disabled.
 *
 * "C:etrace dump FILE=..." writes the ring as a binary file:
 *   header: "ETRC" magic | version u16 | count u32 | reserved
 *   then count × {tsc u64, event u16, pad u16, arg0 u32, arg1 u32}
 * tools/etrace_decode.py renders it as text.
 */

#include "diag.h"
#include "../dos/vfs.h"
#include "../irq/irq.h"
#include "../boot/kprint.h"

#define ETRACE_RING_CAP 16384    /* 16384 × 16 B = 256 KB BSS */

static EtraceRec g_ring[ETRACE_RING_CAP];
static volatile uint32_t g_head;        /* write cursor (next slot) */
static volatile uint32_t g_dropped;
static volatile uint32_t g_mask;        /* ETRACE_CLS_* bits */

static inline int etrace_class_of(uint16_t event)
{
    switch (event) {
    case ETRACE_IRQ_ENTER:
    case ETRACE_IRQ_EXIT:  return ETRACE_CLS_IRQ;
    case ETRACE_SCHED:     return ETRACE_CLS_SCHED;
    case ETRACE_SIGNAL:    return ETRACE_CLS_SIGNAL;
    case ETRACE_DOPKT:     return ETRACE_CLS_DOS;
    case ETRACE_PKT_TX:
    case ETRACE_PKT_RX:    return ETRACE_CLS_NET;
    default:               return 0xFFFFFFFFu;
    }
}

void Etrace_Emit(uint16_t event, uint32_t arg0, uint32_t arg1)
{
    uint32_t m = g_mask;
    if (!(m & etrace_class_of(event))) return;

    /* Single-producer-per-IRQ-nesting assumption is violated when an IRQ
     * nests mid-store; the ring tolerates a torn slot (it just becomes a
     * dropped record).  irq_save keeps the cursor update atomic. */
    uint64_t fl = irq_save();
    uint32_t slot = g_head % ETRACE_RING_CAP;
    g_head++;
    irq_restore(fl);

    EtraceRec *r = &g_ring[slot];
    r->tsc   = diag_rdtsc();
    r->event = event;
    r->_pad  = 0;
    r->arg0  = arg0;
    r->arg1  = arg1;
}

void     Etrace_SetMask(uint32_t cls_mask) { g_mask = cls_mask; }
uint32_t Etrace_Mask(void)                 { return g_mask; }

uint32_t Etrace_Count(void)
{
    return (g_head < ETRACE_RING_CAP) ? g_head : ETRACE_RING_CAP;
}
uint32_t Etrace_Dropped(void) { return g_dropped; }

int Etrace_DumpFile(const char *path)
{
    VfsFile f;
    if (!VFS_Open(&f, path, VFS_WRITE | VFS_CREATE | VFS_TRUNC))
        return 0;

    uint32_t count = Etrace_Count();
    uint32_t first = (g_head < ETRACE_RING_CAP) ? 0
                     : (uint32_t)(g_head % ETRACE_RING_CAP);

    struct { char magic[4]; uint16_t ver; uint16_t rsv;
             uint32_t count; } hdr = { { 'E','T','R','C' }, 1, 0, count };
    VFS_Write(&f, (const uint8_t *)&hdr, sizeof(hdr));

    for (uint32_t i = 0; i < count; i++) {
        EtraceRec *r = &g_ring[(first + i) % ETRACE_RING_CAP];
        VFS_Write(&f, (const uint8_t *)r, sizeof(EtraceRec));
    }
    VFS_Close(&f);
    return 1;
}

static const char *ev_name(uint16_t ev)
{
    switch (ev) {
    case ETRACE_IRQ_ENTER: return "irq+";
    case ETRACE_IRQ_EXIT:  return "irq-";
    case ETRACE_SCHED:     return "sched";
    case ETRACE_SIGNAL:    return "signal";
    case ETRACE_DOPKT:     return "dopkt";
    case ETRACE_PKT_TX:    return "pkt-tx";
    case ETRACE_PKT_RX:    return "pkt-rx";
    default:               return "??";
    }
}

void Etrace_Tail(void *ctx, DiagEmitFn emit, uint32_t n)
{
    DiagLine l;
    uint32_t count = Etrace_Count();
    uint32_t first = (g_head < ETRACE_RING_CAP) ? 0
                     : (uint32_t)(g_head % ETRACE_RING_CAP);
    if (n > count) n = count;
    uint32_t start = count - n;

    for (uint32_t i = start; i < count; i++) {
        EtraceRec *r = &g_ring[(first + i) % ETRACE_RING_CAP];
        dl_reset(&l);
        dl_add(&l, " "); dl_dec(&l, i); dl_pad(&l, 8);
        dl_add(&l, ev_name(r->event)); dl_pad(&l, 16);
        dl_add(&l, "a0="); dl_hex(&l, r->arg0);
        dl_add(&l, " a1="); dl_hex(&l, r->arg1);
        dl_add(&l, " tsc="); dl_hex(&l, r->tsc);
        dl_emit(&l, ctx, emit);
    }
}
