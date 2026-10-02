/* cmd_failalloc.c — C:failalloc — deterministic alloc-failure injection
 * (UAOS-211)
 *
 *   failalloc                status
 *   failalloc ON RATE=n      1-in-n allocations fail (both heaps)
 *   failalloc ON AFTER=n     fail every alloc after n successes
 *   failalloc ON SEED=n      deterministic LCG seed
 *   failalloc OFF
 *
 * Both guest AllocMem (m68k) and the x64 heap path consult the
 * injector.  Reproduces the unchecked-NULL bug class deterministically.
 */

#include "cmd_internal.h"
#include "../dbg/diag.h"

static void fa_emit(void *ctx, const char *line)
{
    CMD_PRINT((NativeCmdCtx *)ctx, line);
}

void Cmd_Failalloc(NativeCmdCtx *ctx, const char *args)
{
    if (args && cmd_kw_find(args, "off")) {
        Failalloc_Config(0, 0, 0, 0);
        PRINT("failalloc: off");
        return;
    }
    if (args && cmd_kw_find(args, "on")) {
        uint32_t rate = 0, after = 0, seed = 1;
        const char *p; uint64_t v;
        if ((p = cmd_kv_find(args, "rate"))  && cmd_parse_uint(p, &v)) rate  = (uint32_t)v;
        if ((p = cmd_kv_find(args, "after")) && cmd_parse_uint(p, &v)) after = (uint32_t)v;
        if ((p = cmd_kv_find(args, "seed"))  && cmd_parse_uint(p, &v)) seed  = (uint32_t)v;
        if (!rate && !after) { PRINT("failalloc: need RATE=n or AFTER=n"); return; }
        Failalloc_Config(1, rate, after, seed);
        Failalloc_Status(ctx, fa_emit);
        return;
    }
    Failalloc_Status(ctx, fa_emit);
    PRINT("usage: failalloc ON RATE=n|AFTER=n [SEED=n] | OFF");
}
