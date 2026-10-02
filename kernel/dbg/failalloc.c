/* failalloc.c — deterministic allocation-failure injection (UAOS-211)
 *
 * Unchecked AllocMem/heap returns are a classic UAOS bug class (UAOS-162:
 * a NULL-returning stub silently broke bridge init).  This module makes
 * allocation paths fail on a deterministic schedule so those code paths
 * get exercised without a debugger:
 *
 *   failalloc on rate=3          every 3rd alloc fails (after warm-up)
 *   failalloc on rate=0          every alloc fails
 *   failalloc on after=1000      skip first 1000 allocs, then apply rate
 *   failalloc seed=12345 on      deterministic xorshift32 sequence
 *   failalloc off                disable
 *
 * Injected failures are logged once per failure to klog [exec] with the
 * alloc ordinal so a reboot reproduces the identical failing call.
 */

#include "diag.h"
#include "../boot/kprint.h"

static volatile int      g_fa_on;
static volatile uint32_t g_fa_rate   = 3;       /* 1-in-N */
static volatile uint32_t g_fa_after;
static volatile uint32_t g_fa_seed   = 0x9E3779B9u;
static volatile uint32_t g_fa_rng    = 0x9E3779B9u;
static volatile uint64_t g_fa_allocs;
static volatile uint64_t g_fa_fails;

void Failalloc_Config(int on, uint32_t rate, uint32_t after, uint32_t seed)
{
    if (seed) { g_fa_seed = seed; }
    g_fa_rng   = g_fa_seed;
    g_fa_rate  = rate;
    g_fa_after = after;
    g_fa_allocs = 0;
    g_fa_fails  = 0;
    g_fa_on    = on;
}

static uint32_t fa_next(void)
{
    uint32_t x = g_fa_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_fa_rng = x;
    return x;
}

int Failalloc_ShouldFail(int pool)
{
    if (!g_fa_on) return 0;

    uint64_t n = ++g_fa_allocs;
    if (n <= g_fa_after) return 0;

    int fail;
    if (g_fa_rate == 0) {
        fail = 1;
    } else {
        fail = (fa_next() % g_fa_rate) == 0;
    }
    if (!fail) return 0;

    g_fa_fails++;
    kprint("[failalloc] injected failure alloc#");
    kprinthex(n);
    kprint(pool == FAILALLOC_X64 ? " pool=x64" : " pool=guest");
    kprint("\n");
    return 1;
}

void Failalloc_Status(void *ctx, DiagEmitFn emit)
{
    DiagLine l;
    dl_reset(&l);
    dl_add(&l, "failalloc: ");
    dl_add(&l, g_fa_on ? "ON" : "off");
    dl_add(&l, " rate=1/");
    dl_dec(&l, g_fa_rate ? g_fa_rate : 1);
    if (!g_fa_rate) dl_add(&l, "(every)");
    dl_add(&l, " after="); dl_dec(&l, g_fa_after);
    dl_add(&l, " seed="); dl_hex(&l, g_fa_seed);
    dl_emit(&l, ctx, emit);

    dl_add(&l, "allocs seen="); dl_dec(&l, g_fa_allocs);
    dl_add(&l, " injected fails="); dl_dec(&l, g_fa_fails);
    dl_emit(&l, ctx, emit);
}
