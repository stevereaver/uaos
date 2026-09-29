/*
 * entropy.c — kernel entropy source
 *
 * Priority order:
 *   1. RDSEED (CPUID.07H:EBX[18]) — seed-grade, use when present
 *   2. RDRAND (CPUID.01H:ECX[30]) — DRBG output, retries on underflow
 *   3. RDTSC timing jitter folded into a xorshift pool, seeded from
 *      PIT ticks, RDTSC, and a per-carry counter — weak fallback only
 *      (no real entropy guarantee; documented for TLS users).
 *
 * entropy_init() runs CPUID detection once at boot; the fill path is
 * safe to call before init (it lazily detects).
 */
#include "entropy.h"

extern volatile uint64_t g_pit_ticks;   /* 100 Hz — uaos_kernel_main.c */

static int g_detected = 0;
static int g_have_rdrand = 0;
static int g_have_rdseed = 0;

static inline void cpuid(uint32_t leaf, uint32_t subleaf,
                         uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(subleaf));
}

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static int hw_u64(uint64_t *out)
{
    uint64_t v;
    uint8_t ok;
    int i;

    if (g_have_rdseed) {
        for (i = 0; i < 8; i++) {
            __asm__ volatile ("rdseed %0; setc %1" : "=r"(v), "=qm"(ok));
            if (ok) { *out = v; return 1; }
        }
    }
    if (g_have_rdrand) {
        for (i = 0; i < 8; i++) {
            __asm__ volatile ("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
            if (ok) { *out = v; return 1; }
        }
    }
    return 0;
}

/* xorshift64* fallback pool — mixed with fresh TSC jitter on each call */
static uint64_t g_pool = 0x9E3779B97F4A7C15ULL;
static uint64_t g_jitter_ctr = 0;

static uint64_t jitter_u64(void)
{
    uint64_t x, prev, acc = 0;
    int i;

    /* Fold TSC delta bits (low bits carry timing jitter) 64 times. */
    prev = rdtsc();
    for (i = 0; i < 64; i++) {
        uint64_t now = rdtsc();
        acc ^= (now - prev) << (i & 7);
        acc = (acc << 1) | (acc >> 63);
        prev = now;
    }
    g_pool ^= acc ^ g_pit_ticks ^ (++g_jitter_ctr * 0x2545F4914F6CDD1DULL)
              ^ (uint64_t)(uintptr_t)&i;
    /* xorshift64* */
    x = g_pool;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    g_pool = x;
    return x * 0x2545F4914F6CDD1DULL;
}

void entropy_init(void)
{
    uint32_t a, b, c, d;

    cpuid(0, 0, &a, &b, &c, &d);
    if (a >= 1) {
        cpuid(1, 0, &a, &b, &c, &d);
        g_have_rdrand = (c >> 30) & 1;
    }
    if (a >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        g_have_rdseed = (b >> 18) & 1;
    }
    /* Stir boot-time state into the fallback pool. */
    g_pool ^= rdtsc() ^ g_pit_ticks;
    g_detected = 1;
}

int entropy_hw_available(void)
{
    if (!g_detected)
        entropy_init();
    return g_have_rdrand || g_have_rdseed;
}

uint32_t entropy_fill(void *buf, uint32_t len)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;

    if (!buf)
        return 0;
    if (!g_detected)
        entropy_init();

    while (done < len) {
        uint64_t v;
        int k;

        if (!hw_u64(&v))
            v = jitter_u64();
        for (k = 0; k < 8 && done < len; k++) {
            p[done++] = (uint8_t)(v & 0xFF);
            v >>= 8;
        }
    }
    return done;
}
