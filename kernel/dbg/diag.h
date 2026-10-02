/* diag.h — UAOS kernel diagnostics: shared emit plumbing + module APIs
 *
 * All diag dump functions share the emit-callback pattern: the producer
 * builds a line and hands it to emit(ctx, line), so the same dump works
 * for the shell (ctx->print), the serial console (uart_puts) and klog.
 *
 * DiagLine is a tiny append-style line builder for freestanding code —
 * no printf dependency, safe from IRQ and early-boot contexts.
 */

#ifndef UAOS_DIAG_H
#define UAOS_DIAG_H

#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Emit callback + line builder
 * ------------------------------------------------------------------------- */
typedef void (*DiagEmitFn)(void *ctx, const char *line);

typedef struct {
    char buf[120];
    int  len;
} DiagLine;

static inline void dl_reset(DiagLine *l) { l->len = 0; l->buf[0] = '\0'; }

static inline void dl_add(DiagLine *l, const char *s)
{
    while (*s && l->len < (int)sizeof(l->buf) - 1)
        l->buf[l->len++] = *s++;
    l->buf[l->len] = '\0';
}

static inline void dl_ch(DiagLine *l, char c)
{
    if (l->len < (int)sizeof(l->buf) - 1) {
        l->buf[l->len++] = c;
        l->buf[l->len] = '\0';
    }
}

static inline void dl_dec(DiagLine *l, uint64_t v)
{
    char tmp[24];
    int n = 0;
    if (!v) { dl_ch(l, '0'); return; }
    while (v && n < 23) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) dl_ch(l, tmp[--n]);
}

static inline void dl_sdec(DiagLine *l, int64_t v)
{
    if (v < 0) { dl_ch(l, '-'); dl_dec(l, (uint64_t)(-v)); return; }
    dl_dec(l, (uint64_t)v);
}

static inline void dl_hex(DiagLine *l, uint64_t v)
{
    char tmp[20];
    int n = 0;
    if (!v) { dl_add(l, "0x0"); return; }
    while (v && n < 18) { tmp[n++] = "0123456789abcdef"[v & 15]; v >>= 4; }
    dl_add(l, "0x");
    while (n) dl_ch(l, tmp[--n]);
}

/* Right-align the last-appended field: pad with spaces until len>=w */
static inline void dl_pad(DiagLine *l, int w)
{
    while (l->len < w) dl_ch(l, ' ');
}

static inline void dl_emit(DiagLine *l, void *ctx, DiagEmitFn emit)
{
    emit(ctx, l->buf);
    dl_reset(l);
}

/* TSC read — used by tickmon/prof/etrace sampling. */
static inline uint64_t diag_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* -------------------------------------------------------------------------
 * watchdog.c — scheduler/tick stall detector (UAOS-198)
 * ------------------------------------------------------------------------- */
void     Watchdog_SetBudget(uint32_t ms);   /* 0 = off */
uint32_t Watchdog_Budget(void);
int      Watchdog_Enabled(void);
void     Watchdog_Tick(void);        /* call from PIT_IRQHandler */
void     Watchdog_RtcSecond(void);   /* call from RTC IRQ8 handler  */
void     Watchdog_Dump(void *ctx, DiagEmitFn emit);
void     Watchdog_Test(void);        /* artificial stall — C:watchdog TEST */

/* -------------------------------------------------------------------------
 * tickmon.c — tick/IRQ timing instrumentation (UAOS-208)
 * ------------------------------------------------------------------------- */
#define TICKMON_LAT_BUCKETS 16    /* log2(cycles) buckets */

void Tickmon_PitTick(uint64_t tsc);              /* from PIT_IRQHandler      */
void Tickmon_IrqDur(uint8_t vector, uint64_t cycles); /* from ISR_Dispatch   */

typedef struct {
    uint64_t pit_last_tsc;
    uint64_t pit_min_delta, pit_max_delta, pit_sum_delta, pit_samples;
    uint64_t irq_hist[TICKMON_LAT_BUCKETS];      /* all vectors, log2 cycles */
    uint64_t vec_max_cycles[256];                /* worst single dispatch    */
} TickmonStats;
void Tickmon_Snapshot(TickmonStats *out);
void Tickmon_Clear(void);
uint64_t Tickmon_TscHz(void);   /* self-calibrated TSC Hz (0 early) */

/* -------------------------------------------------------------------------
 * etrace.c — binary event trace ring (UAOS-209)
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t tsc;
    uint16_t event;
    uint16_t _pad;
    uint32_t arg0;
    uint32_t arg1;
} EtraceRec;   /* 20 bytes on disk; packed to 16 for the file format */

/* Event classes — mask bits */
#define ETRACE_CLS_IRQ    (1u << 0)   /* IRQ enter/exit            */
#define ETRACE_CLS_SCHED  (1u << 1)   /* context switches          */
#define ETRACE_CLS_SIGNAL (1u << 2)   /* Signal()                  */
#define ETRACE_CLS_DOS    (1u << 3)   /* DoPkt dispatch            */
#define ETRACE_CLS_NET    (1u << 4)   /* netdev TX/RX frames       */

/* Event ids */
enum {
    ETRACE_IRQ_ENTER = 1, ETRACE_IRQ_EXIT,
    ETRACE_SCHED, ETRACE_SIGNAL, ETRACE_DOPKT,
    ETRACE_PKT_TX, ETRACE_PKT_RX,
};

void     Etrace_Emit(uint16_t event, uint32_t arg0, uint32_t arg1);
void     Etrace_SetMask(uint32_t cls_mask);   /* 0 = off */
uint32_t Etrace_Mask(void);
uint32_t Etrace_Count(void);                  /* records in ring */
uint32_t Etrace_Dropped(void);
int      Etrace_DumpFile(const char *path);   /* binary -> VFS, 1 ok */
void     Etrace_Tail(void *ctx, DiagEmitFn emit, uint32_t n); /* text tail */

/* -------------------------------------------------------------------------
 * prof.c — PIT-sampled RIP profiler (UAOS-210)
 * ------------------------------------------------------------------------- */
void Prof_Start(void);
void Prof_Stop(void);
int  Prof_Running(void);
void Prof_Tick(void);   /* call from PIT_IRQHandler — samples stashed frame */
void Prof_Report(void *ctx, DiagEmitFn emit, int top_n);
int  Prof_DumpFile(const char *path);          /* "rip taskidx count" lines */
uint32_t Prof_SampleCount(void);

/* -------------------------------------------------------------------------
 * failalloc.c — deterministic allocation fault injection (UAOS-211)
 * ------------------------------------------------------------------------- */
#define FAILALLOC_GUEST 0   /* exec.library AllocMem (guest heap)   */
#define FAILALLOC_X64   1   /* x64 userspace heap                   */

void Failalloc_Config(int on, uint32_t rate, uint32_t after, uint32_t seed);
int  Failalloc_ShouldFail(int pool);        /* call at top of alloc paths */
void Failalloc_Status(void *ctx, DiagEmitFn emit);

#endif /* UAOS_DIAG_H */
