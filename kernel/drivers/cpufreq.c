/* cpufreq.c — EIST P-state scaling + ondemand governor (UAOS-272)
 *
 * Target: Intel Core 2 Duo (Merom model 0x0F, Penryn model 0x17) as in
 * the MBP4,1.  On these parts MSR_IA32_PERF_CTL[15:0] takes a
 * {FID[15:8], VID[7:0]} operating-point word whose encoding differs
 * between models (Penryn uses half-ratio FID units), so firmware _PSS
 * ctl/sts values are passed through verbatim and never interpreted.
 *
 * Table source order:
 *   1. ACPI _PSS from the DSDT — a minimal AML scanner for
 *      `Name (_PSS, Package () { Package {freq,power,tlat,blat,ctl,sts} ... })`
 *      accepting only pure-constant packages.
 *   2. Synthesised 2-state table — HIGH = the boot PERF_STATUS word
 *      (whatever firmware left running), LOW = model minimum FID with
 *      the same VID.  Same-VID writes can never undervolt the core, and
 *      the frequency drop alone still cuts dynamic power ~half.
 *   3. No EIST at all → TM1 duty modulation (IA32_THERM_CONTROL
 *      bit4 + bits[3:1] = duty/12.5%) as a coarse throttle.
 *
 * C1E is deliberately NOT touched: MSR_POWER_CTL (0x1FC) is a
 * Nehalem+/Atom MSR — reading it #GPs on Core 2 (observed as a boot
 * panic on the MBP4,1).  Package C-states on Core 2 are ACPI _CST
 * driven, which is out of scope here.
 *
 * Safety: MSR writes happen only on GenuineIntel family-6 models 0x0F
 * or 0x17 with EST enabled and unlocked.  QEMU/VirtualBox expose no EST
 * bit and no _PSS, so the driver stays inert there — the busy% sampler
 * still runs because the menubar meter needs it on every target.
 */

#include "cpufreq.h"

extern volatile uint64_t g_pit_ticks;   /* 100 Hz timebase (uaos_kernel_main.c) */

#include "../boot/kprint.h"
#include "../boot/mb2mod.h"
#include "../irq/acpi.h"
#include "../irq/irq.h"
#include "../exec/task.h"

#define MSR_IA32_PERF_STATUS   0x198
#define MSR_IA32_PERF_CTL      0x199
#define MSR_IA32_THERM_CTL     0x19A
#define MSR_IA32_MISC_ENABLE   0x1A0
#define MSR_POWER_CTL          0x1FC

#define MISC_ENABLE_EIST       (1u << 16)
#define MISC_ENABLE_EIST_LOCK  (1u << 20)
#define THERM_CTL_MOD_EN       (1u << 4)
/* MSR_POWER_CTL (0x1FC) / C1E bit1 — NOT present on Core 2; see header. */

/* Governor — evaluated every CPUFREQ_WIN_TICKS PIT ticks. */
#define CPUFREQ_WIN_TICKS   10      /* 100 ms at 100 Hz               */
#define CPUFREQ_UP_PCT      25      /* busy% that ramps to max        */
#define CPUFREQ_DOWN_PCT    10      /* busy% that counts as idle      */
#define CPUFREQ_DOWN_WINS   8       /* ~0.8 s idle before downshift   */

typedef struct {
    uint32_t mhz;
    uint32_t ctl;
    uint32_t sts;
} PState;

/* ------------------------------------------------------------------ */
/* CPUID / MSR primitives                                              */
/* ------------------------------------------------------------------ */

static inline void cpuid1(uint32_t leaf, uint32_t *a, uint32_t *b,
                          uint32_t *c, uint32_t *d)
{
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(0));
}

static inline uint64_t rdmsr64(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr64(uint32_t msr, uint64_t val)
{
    __asm__ volatile ("wrmsr" :: "a"((uint32_t)val),
                                "d"((uint32_t)(val >> 32)), "c"(msr));
}

/* ------------------------------------------------------------------ */
/* Minimal AML helpers — constant-int terms and packages only           */
/* ------------------------------------------------------------------ */

/* AML PkgLength: lead bits7:6 = follow-byte count; count 0 → bits5:0 is
 * the length, else bits3:0 are the low nibble and the follow bytes add
 * the higher bits.  Returns bytes consumed (0 on malformed input). */
static uint32_t aml_pkglen(const uint8_t *p, const uint8_t *end,
                           uint32_t *len_out)
{
    if (p >= end) return 0;
    uint8_t lead = *p;
    int n = lead >> 6;
    if (p + 1 + n > end) return 0;
    uint32_t v;
    if (n == 0) {
        v = lead & 0x3F;
    } else {
        v = lead & 0x0F;
        for (int i = 0; i < n; i++)
            v |= (uint32_t)p[1 + i] << (4 + 8 * i);
    }
    *len_out = v;
    return 1 + n;
}

/* Decode one AML integer-constant term.  Returns bytes consumed or 0
 * when the term is not a plain constant — _PSS bodies we accept are
 * pure data. */
static uint32_t aml_int(const uint8_t *p, const uint8_t *end,
                        uint64_t *out)
{
    if (p >= end) return 0;
    switch (p[0]) {
    case 0x00: *out = 0; return 1;
    case 0x01: *out = 1; return 1;
    case 0x0A:                          /* BytePrefix */
        if (p + 2 > end) return 0;
        *out = p[1]; return 2;
    case 0x0B:                          /* WordPrefix */
        if (p + 3 > end) return 0;
        *out = (uint64_t)p[1] | ((uint64_t)p[2] << 8); return 3;
    case 0x0C:                          /* DWordPrefix */
        if (p + 5 > end) return 0;
        *out = (uint64_t)p[1]        | ((uint64_t)p[2] << 8) |
               ((uint64_t)p[3] << 16) | ((uint64_t)p[4] << 24);
        return 5;
    case 0x0E:                          /* QWordPrefix */
        if (p + 9 > end) return 0;
        {   uint64_t v = 0;
            for (int i = 0; i < 8; i++) v |= (uint64_t)p[1 + i] << (8 * i);
            *out = v; return 9; }
    }
    return 0;
}

/* Parse a PackageOp of _PSS sub-packages starting at `p` (on the 0x12 /
 * 0x13 opcode).  Each sub-package is {MHz, mW, tranLat, bmLat, ctl,
 * sts}.  Returns states collected, 0 = unusable. */
static int pss_parse_pkg(const uint8_t *p, const uint8_t *end,
                         PState *st, int max)
{
    if (p >= end || (p[0] != 0x12 && p[0] != 0x13)) return 0;
    int varpkg = (p[0] == 0x13);
    p++;

    uint32_t plen = 0;
    uint32_t used = aml_pkglen(p, end, &plen);
    if (!used) return 0;
    const uint8_t *pkg_start = p;
    p += used;

    /* NumElements: one byte for Package, a TermArg (ByteData) for
     * VarPackage — accept a bare byte only. */
    if (p >= end) return 0;
    uint32_t nelem = *p++;
    (void)varpkg;

    const uint8_t *pkg_end = pkg_start + plen;
    if (pkg_end > end) return 0;
    if (!nelem || nelem > 16) return 0;

    int n = 0;
    for (uint32_t i = 0; i < nelem && p < pkg_end && n < max; i++) {
        if (p[0] != 0x12) return 0;         /* not a sub-package */
        const uint8_t *sp = p + 1;
        uint32_t slen = 0;
        uint32_t sused = aml_pkglen(sp, pkg_end, &slen);
        if (!sused) return 0;
        const uint8_t *sub_start = sp;
        const uint8_t *sub_end   = sub_start + slen;
        sp += sused;
        if (sp >= sub_end) return 0;
        uint32_t nfield = *sp++;            /* field count */
        if (nfield != 6 || sub_end > pkg_end) return 0;

        uint64_t f[6];
        for (int k = 0; k < 6; k++) {
            uint32_t c = aml_int(sp, sub_end, &f[k]);
            if (!c) return 0;
            sp += c;
        }
        st[n].mhz = (uint32_t)f[0];
        st[n].ctl = (uint32_t)f[4];
        st[n].sts = (uint32_t)f[5];
        n++;
        p = sub_end;
    }
    return n;
}

static CpuFreqInfo g_cf;

/* Structural sanity check for a parsed PSS-like table: at least two
 * entries, sane MHz (100..6000), ordered descending, non-zero ctl. */
static int pss_sane(const PState *st, int n)
{
    if (n < 2) return 0;
    int desc = st[0].mhz >= st[n - 1].mhz;
    for (int i = 0; i < n; i++) {
        uint32_t fid, vid;
        if (st[i].mhz < 100 || st[i].mhz > 6000 || !st[i].ctl)
            return 0;
        /* ctl goes verbatim to PERF_CTL — require the Core 2 shape
         * (upper 16 bits clear, FID 4..0x40, VID 0x0C..0x40) so a
         * misidentified package can't feed a garbage VID to the MSR. */
        if (st[i].ctl >> 16) return 0;
        fid = (st[i].ctl >> 8) & 0xFF;
        vid = st[i].ctl & 0xFF;
        if (fid < 4 || fid > 0x40 || vid < 0x0C || vid > 0x40)
            return 0;
        if (i && desc  && st[i].mhz > st[i - 1].mhz) return 0;
        if (i && !desc && st[i].mhz < st[i - 1].mhz) return 0;
    }
    return 1;
}

/* Structural fallback: Apple may keep the P-state package under a
 * private name (e.g. `TSSI`) and copy it into `_PSS` from a method, so
 * no `_PSS` declaration exists to find.  Try every PackageOp in the
 * blob and accept the first one that parses and passes pss_sane(). */
static int scan_pkg_any(const uint8_t *aml, uint32_t len, PState *st,
                        int max)
{
    const uint8_t *end = aml + len;
    for (const uint8_t *p = aml; p < end; p++) {
        if (p[0] != 0x12 && p[0] != 0x13) continue;
        int n = pss_parse_pkg(p, end, st, max);
        if (pss_sane(st, n)) return n;
    }
    return 0;
}

/* Find `_PSS` in an AML blob.  Declaration forms handled:
 *   Name (_PSS, Package() {...})                — 0x08 '_PSS' <pkg>
 *   Method (_PSS, ...) { Return (Package(){}) } — 0x14 <pkglen> '_PSS'
 *     <flags> ... 0xA4 <pkg>
 *   Alias (PSS, \_PR.CPUx._PSS) + Name (PSS, Package(){}) — Apple style:
 *     the '_PSS' string only ever appears as the alias target, so the
 *     nearest preceding AliasOp (0x06) names the real object, which we
 *     then look up as `Name (src, Package){}` in the same table.
 * Only pure-constant packages parse; method-computed tables and
 * non-package returns are skipped.  *hits counts `_PSS` occurrences
 * seen but not parseable, for C:cpu diagnostics. */
static int scan_pss(const uint8_t *aml, uint32_t len, PState *st, int max,
                    uint32_t *hits)
{
    const uint8_t *end = aml + len;
    for (const uint8_t *p = aml; p + 4 <= end; p++) {
        if (p[0] != '_' || p[1] != 'P' || p[2] != 'S' || p[3] != 'S')
            continue;
        (*hits)++;
        if (g_cf.dbg_n < 4)
            g_cf.dbg_addr[g_cf.dbg_n++] = (uint32_t)(uintptr_t)p;

        /* Form 1: Name (_PSS, Package() {...}) */
        if (p > aml && p[-1] == 0x08) {
            int n = pss_parse_pkg(p + 4, end, st, max);
            if (n >= 2) return n;
            continue;
        }

        /* Form 2: Method (_PSS, ...) — the MethodOp 0x14 sits before a
         * 1-4 byte PkgLength; the name lands right after it. */
        for (int k = 2; k <= 5 && p - k >= aml; k++) {
            const uint8_t *m = p - k;
            if (m[0] != 0x14) continue;
            uint32_t mlen = 0;
            uint32_t used = aml_pkglen(m + 1, end, &mlen);
            if (!used || m + 1 + used != p) continue;
            const uint8_t *body     = p + 5;    /* past name + flags byte */
            const uint8_t *body_end = m + 1 + mlen;
            if (body_end > end) break;
            /* First Return inside the method body — inline Package or
             * `Return (NAME)` where NAME is a Name()d package. */
            for (const uint8_t *r = body; r + 1 < body_end; r++) {
                if (r[0] != 0xA4) continue;
                if (r[1] == 0x12 || r[1] == 0x13) {
                    int n = pss_parse_pkg(r + 1, body_end, st, max);
                    if (n >= 2) return n;
                } else if ((r[1] >= 'A' && r[1] <= 'Z') || r[1] == '_') {
                    /* Return (Name) — resolve the simple-name target. */
                    const uint8_t *src = r + 1;
                    if (src + 4 > body_end) break;
                    for (const uint8_t *s2 = aml; s2 + 6 <= end; s2++) {
                        if (s2[0] != 0x08 || s2[1] != src[0] ||
                            s2[2] != src[1] || s2[3] != src[2] ||
                            s2[4] != src[3])
                            continue;
                        int n = pss_parse_pkg(s2 + 5, end, st, max);
                        if (n >= 2) return n;
                    }
                }
                break;
            }
            break;
        }

        /* Form 3: Alias (src, <path>._PSS) — nearest preceding AliasOp
         * names the real package.  Resolve `Name (src, Package){}` in
         * the same table. */
        for (const uint8_t *q = p - 1; q > aml && q > p - 24; q--) {
            if (*q != 0x06) continue;
            const uint8_t *src = q + 1;          /* src NameSeg (4 B) */
            if (src + 4 > p) break;
            for (const uint8_t *s2 = aml; s2 + 6 <= end; s2++) {
                if (s2[0] != 0x08 || s2[1] != src[0] || s2[2] != src[1] ||
                    s2[3] != src[2] || s2[4] != src[3])
                    continue;
                int n = pss_parse_pkg(s2 + 5, end, st, max);
                if (n >= 2) return n;
            }
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Driver state                                                        */
/* ------------------------------------------------------------------ */

static PState      g_states[CPUFREQ_MAX_STATES];

static volatile uint64_t g_win_last_tick  = 0;
static volatile uint64_t g_win_last_idle  = 0;
static volatile uint32_t g_win_count      = 0;
static volatile uint32_t g_idle_wins      = 0;
static UaosTask         *g_idle_task      = NULL;
static int               g_msr_ok         = 0;   /* whitelisted model */
static int               g_tm1_duty       = -1;  /* last TM1 duty written */

/* Model LFM FID — integer-ratio units (fid × bus MHz).  Same VID as
 * the running state, so the write can never undervolt. */
static uint32_t synth_low_ctl(int model, uint32_t boot_ctl)
{
    (void)model;
    uint32_t vid = boot_ctl & 0xFF;
    return (0x06 << 8) | vid;
}

/* FID byte → bus multiplier: bits [4:0] integer ratio, bit 6 (0x40)
 * the +0.5 step — e.g. 0x0C = 12.0×, 0x4C = 12.5× (Core 2 encoding,
 * confirmed against Penryn _PSS tables: ctl = fid<<8 | vid). */
static uint32_t fid_mhz(uint32_t ctl, uint32_t bus_mhz)
{
    uint32_t fid = (ctl >> 8) & 0xFF;
    uint32_t mult2 = (fid & 0x1F) * 2 + ((fid & 0x40) ? 1 : 0);
    return mult2 * bus_mhz / 2;
}

/* MHz hint for a synthesised entry — cosmetic display only. */
static uint32_t synth_mhz(int model, uint32_t ctl)
{
    return fid_mhz(ctl, (model == 0x17) ? 200 : 167);
}

/* Penryn mobile ladder (Core 2 Duo T9300 in the MBP4,1: 200 MHz bus,
 * multipliers 6.0×–12.5×).  Core 2 VID code = 12.5 mV steps above
 * 712.5 mV — 0x17 = 1.00 V (the MBP4,1's own boot VID, firmware-
 * validated), 0x24 = 1.16 V for the top state.  VIDs are clamped at
 * init to never go below the boot VID. */
static const struct { uint8_t fid; uint8_t vid; } penryn_ladder[] = {
    { 0x4C, 0x24 },   /* 12.5× = 2500 MHz */
    { 0x0C, 0x22 },   /* 12.0× = 2400 MHz */
    { 0x0B, 0x20 },   /* 2200 */
    { 0x0A, 0x1E },   /* 2000 */
    { 0x09, 0x1C },   /* 1800 */
    { 0x08, 0x1A },   /* 1600 */
    { 0x07, 0x18 },   /* 1400 */
    { 0x06, 0x17 },   /*  6.0× = 1200 MHz (boot VID floor) */
};

static void set_state(int idx)
{
    if (idx < 0 || idx >= g_cf.n_states || idx == g_cf.cur_idx)
        return;
    uint64_t cur = rdmsr64(MSR_IA32_PERF_CTL);
    wrmsr64(MSR_IA32_PERF_CTL,
            (cur & ~0xFFFFull) | (g_states[idx].ctl & 0xFFFF));
    g_cf.cur_idx = idx;
    g_cf.transitions++;
}

/* TM1 fallback: duty cycle = value * 12.5 % (bits [3:1]); bit4 enables. */
static void tm1_set(int duty)
{
    if (duty == g_tm1_duty) return;
    g_tm1_duty = duty;
    uint64_t t = rdmsr64(MSR_IA32_THERM_CTL);
    if (duty <= 0) {
        t &= ~0x1Full;                     /* modulation off, duty 0 */
    } else {
        t = (t & ~0x1Full) | THERM_CTL_MOD_EN | ((uint64_t)duty << 1);
    }
    wrmsr64(MSR_IA32_THERM_CTL, t);
}

/* ------------------------------------------------------------------ */
/* Governor + busy% sampler — called every PIT tick (IRQ context)      */
/* ------------------------------------------------------------------ */

void CpuFreq_Tick(void)
{
    if (++g_win_count < CPUFREQ_WIN_TICKS) return;
    g_win_count = 0;

    UaosTask *idle = g_idle_task;
    if (!idle) {
        idle = Task_FindByName("Idle");
        if (idle) g_idle_task = idle;
    }

    uint64_t now   = g_pit_ticks;
    uint64_t dt    = now - g_win_last_tick;
    uint64_t didle = idle ? idle->cpu_ticks - g_win_last_idle : 0;
    g_win_last_tick = now;
    if (idle) g_win_last_idle = idle->cpu_ticks;
    if (!dt) return;

    uint32_t busy = (didle >= dt) ? 0 : (uint32_t)((dt - didle) * 100 / dt);
    if (busy > 100) busy = 100;
    g_cf.busy_pct = busy;

    if (!g_cf.scale_ok) return;

    if (busy >= CPUFREQ_UP_PCT) {
        g_idle_wins = 0;
        if (g_cf.tm1) tm1_set(0);
        else          set_state(0);            /* table[0] = highest MHz */
    } else if (busy < CPUFREQ_DOWN_PCT) {
        if (++g_idle_wins >= CPUFREQ_DOWN_WINS) {
            g_idle_wins = CPUFREQ_DOWN_WINS;
            if (g_cf.tm1) tm1_set(1);          /* 12.5 % duty           */
            else if (g_cf.n_states > 1)
                set_state(g_cf.n_states - 1);  /* lowest P-state        */
        }
    } else {
        g_idle_wins = 0;
    }
}

uint32_t CpuFreq_BusyPercent(void)
{
    return g_cf.busy_pct;
}

void CpuFreq_GetInfo(CpuFreqInfo *out)
{
    uint64_t fl = irq_save();
    *out = g_cf;
    for (int i = 0; i < g_cf.n_states && i < CPUFREQ_MAX_STATES; i++) {
        out->state_mhz[i] = g_states[i].mhz;
        out->state_ctl[i] = g_states[i].ctl;
        out->state_sts[i] = g_states[i].sts;
    }
    if (g_msr_ok)
        out->perf_status = (uint32_t)(rdmsr64(MSR_IA32_PERF_STATUS) & 0xFFFF);
    irq_restore(fl);
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

void CpuFreq_Init(uint32_t mb2_phys)
{
    for (int i = 0; i < (int)sizeof(g_cf); i++)
        ((uint8_t *)&g_cf)[i] = 0;
    g_cf.cur_idx = -1;

    if (Mb2_CmdlineHas(mb2_phys, "nocpufreq")) {
        kprint("[CPUFREQ] disabled by cmdline (busy% still sampled)\n");
        return;
    }

    uint32_t a, b, c, d;
    cpuid1(1, &a, &b, &c, &d);
    int family = (int)((a >> 8) & 0xF) + (int)((a >> 20) & 0xFF);
    int model  = (int)((a >> 4) & 0xF) | (int)(((a >> 16) & 0xF) << 4);
    g_cf.cpu_family = family;
    g_cf.cpu_model  = model;
    g_cf.est = !!(c & (1u << 7));         /* ECX bit7 = EST  */
    int tm_cap = !!(d & (1u << 22));      /* EDX bit22 = ACPI/TM */

    cpuid1(0, &a, &b, &c, &d);
    if (b != 0x756E6547u || d != 0x49656E69u || c != 0x6C65746Eu) {
        kprint("[CPUFREQ] not GenuineIntel — inactive\n");
        return;
    }

    /* Invariant TSC — recorded for C:cpu only.  Core 2 lacks it (the
     * flag debuted on Nehalem), but every TSC consumer in this kernel is
     * either a floor-delay busy-wait (AHCI/DHCP ms_spin — a slower TSC
     * just waits longer) or a diagnostic delta (tickmon/irqaudit), so
     * scaling is safe without it. */
    cpuid1(0x80000000u, &a, &b, &c, &d);
    if (a >= 0x80000007u) {
        cpuid1(0x80000007u, &a, &b, &c, &d);
        g_cf.inv_tsc = !!(d & (1u << 8));
    }

    /* MSR writes are validated only on the Core 2 family. */
    g_msr_ok = (family == 6 && (model == 0x0F || model == 0x17));
    kprint("[CPUFREQ] fam="); kprintdec((uint32_t)family);
    kprint(" model="); kprinthex((uint64_t)model);
    kprint(" est="); kprintdec((uint32_t)g_cf.est);
    kprint(" invtsc="); kprintdec((uint32_t)g_cf.inv_tsc);
    kprint("\n");

    if (!g_msr_ok) {
        kprint("[CPUFREQ] unsupported model — meter only\n");
        return;
    }

    /* EIST enable — trust the MSR readback, not CPUID.ECX.7: firmware
     * can hide the CPUID bit while leaving MISC_ENABLE.16 writable, and
     * the bit can also read 0 simply because firmware never enabled it.
     * The write only happens when the lock bit (20) is clear; a locked
     * MSR is never written (locked MSRs may #GP). */
    {
        uint64_t m = rdmsr64(MSR_IA32_MISC_ENABLE);
        g_cf.locked = !!(m & MISC_ENABLE_EIST_LOCK);
        if (!g_cf.locked && !(m & MISC_ENABLE_EIST)) {
            wrmsr64(MSR_IA32_MISC_ENABLE, m | MISC_ENABLE_EIST);
            m = rdmsr64(MSR_IA32_MISC_ENABLE);
        }
        g_cf.eist_on = !!(m & MISC_ENABLE_EIST);
        if (g_cf.eist_on)
            g_cf.est = 1;    /* whitelisted model + bit16 set → real EIST */
        else
            kprint("[CPUFREQ] EIST off (locked or rejected)\n");
    }

    /* ---- P-state table ---- */
    g_cf.boot_ctl = (uint32_t)(rdmsr64(MSR_IA32_PERF_STATUS) & 0xFFFF);
    if (g_cf.est) {
        int n = 0;
        /* Scan the DSDT first, then every SSDT — Apple declares the
         * \_PR.CPUx objects (and their _PSS) in SSDTs, not the DSDT. */
        const void *dsdt = ACPI_Dsdt();
        if (dsdt) {
            const uint8_t *hdr = (const uint8_t *)dsdt;
            uint32_t len = *(const uint32_t *)(hdr + 4);
            uint64_t base = (uint64_t)(uintptr_t)hdr;
            if (len > 36 && base + len <= (1ull << 32) && len < (1u << 22)) {
                g_cf.tables_scanned++;
                if (g_cf.dbg_tbln < 8) {
                    g_cf.dbg_tbl[g_cf.dbg_tbln]  = (uint32_t)(uintptr_t)hdr;
                    g_cf.dbg_tlen[g_cf.dbg_tbln] = len;
                    g_cf.dbg_tbln++;
                }
                n = scan_pss(hdr + 36, len - 36, g_states,
                             CPUFREQ_MAX_STATES, &g_cf.pss_hits);
            }
        }
        for (int i = 0; n < 2; i++) {
            const uint8_t *ssdt = (const uint8_t *)ACPI_FindTableN("SSDT", i);
            if (!ssdt) break;
            uint32_t len = *(const uint32_t *)(ssdt + 4);
            uint64_t base = (uint64_t)(uintptr_t)ssdt;
            if (len <= 36 || base + len > (1ull << 32) || len >= (1u << 22))
                continue;
            g_cf.tables_scanned++;
            if (g_cf.dbg_tbln < 8) {
                g_cf.dbg_tbl[g_cf.dbg_tbln]  = (uint32_t)(uintptr_t)ssdt;
                g_cf.dbg_tlen[g_cf.dbg_tbln] = len;
                g_cf.dbg_tbln++;
            }
            n = scan_pss(ssdt + 36, len - 36, g_states,
                         CPUFREQ_MAX_STATES, &g_cf.pss_hits);
        }
        if (n < 2) {
            /* Apple MBP4,1 CpuPm-style SSDT: the package lives under a
             * private name (TSSI) and a method copies it into a scratch
             * `_PSS`, so no `_PSS` declaration exists.  Structural
             * fallback: first PSS-shaped constant package in any table. */
            if (dsdt) {
                const uint8_t *hdr = (const uint8_t *)dsdt;
                uint32_t len = *(const uint32_t *)(hdr + 4);
                if (len > 36 && len < (1u << 22))
                    n = scan_pkg_any(hdr + 36, len - 36, g_states,
                                     CPUFREQ_MAX_STATES);
            }
            for (int i = 0; n < 2; i++) {
                const uint8_t *ssdt =
                    (const uint8_t *)ACPI_FindTableN("SSDT", i);
                if (!ssdt) break;
                uint32_t len = *(const uint32_t *)(ssdt + 4);
                if (len <= 36 || len >= (1u << 22))
                    continue;
                n = scan_pkg_any(ssdt + 36, len - 36, g_states,
                                 CPUFREQ_MAX_STATES);
            }
            if (n >= 2)
                g_cf.table_source = 3;      /* heuristic match */
        }
        if (n >= 2) {
            if (!g_cf.table_source)
                g_cf.table_source = 1;
            /* Sort by MHz descending — firmware order is trusted but
             * not assumed. */
            for (int i = 1; i < n; i++) {
                PState t = g_states[i]; int j = i - 1;
                while (j >= 0 && g_states[j].mhz < t.mhz) {
                    g_states[j + 1] = g_states[j]; j--;
                }
                g_states[j + 1] = t;
            }
        } else if (model == 0x17) {
            /* Penryn: no _PSS exists on this firmware at all — the only
             * SSDT is Apple's _TSS (duty-throttling) table; macOS drove
             * EIST via ACPI_SMC, not ACPI.  Use the known-good T9300
             * ladder; every state's VID is clamped to >= the boot VID
             * so nothing can undervolt below the firmware-validated
             * floor. */
            uint32_t bvid = g_cf.boot_ctl & 0xFF;
            n = 0;
            for (int i = 0; i < (int)(sizeof(penryn_ladder) /
                                      sizeof(penryn_ladder[0])) &&
                            n < CPUFREQ_MAX_STATES; i++) {
                uint32_t vid = penryn_ladder[i].vid;
                if (vid < bvid) vid = bvid;
                uint32_t ctl = ((uint32_t)penryn_ladder[i].fid << 8) | vid;
                g_states[n].ctl = ctl;
                g_states[n].sts = ctl;
                g_states[n].mhz = fid_mhz(ctl, 200);
                n++;
            }
            g_cf.table_source = 2;
            kprint("[CPUFREQ] no _PSS — Penryn ladder\n");
        } else {
            /* Synthesised fallback: the boot PERF_STATUS state plus the
             * model LFM at the same VID — same-VID writes can never
             * undervolt.  Ordered by FID descending (firmware may leave
             * the CPU parked below LFM at boot — observed fid=0x06 on
             * the MBP4,1 — so boot is NOT assumed to be the top state). */
            uint32_t boot  = g_cf.boot_ctl;
            uint32_t low   = synth_low_ctl(model, boot);
            uint32_t bfid  = (boot >> 8) & 0xFF;
            uint32_t lfid  = (low  >> 8) & 0xFF;
            uint32_t hi    = (bfid >= lfid) ? boot : low;
            uint32_t lo    = (bfid >= lfid) ? low  : boot;
            n = 0;
            g_states[n].ctl = hi; g_states[n].sts = hi;
            g_states[n].mhz = synth_mhz(model, hi); n++;
            if (lo != hi) {
                g_states[n].ctl = lo; g_states[n].sts = lo;
                g_states[n].mhz = synth_mhz(model, lo); n++;
            }
            g_cf.table_source = 2;
            kprint("[CPUFREQ] no _PSS — synthesised table\n");
        }
        g_cf.n_states = n;
        g_cf.scale_ok = 1;
        for (int i = 0; i < n; i++) {
            kprint("[CPUFREQ] P"); kprintdec((uint32_t)i);
            kprint(" mhz="); kprintdec(g_states[i].mhz);
            kprint(" ctl="); kprinthex(g_states[i].ctl);
            kprint(" sts="); kprinthex(g_states[i].sts);
            kprint("\n");
        }
    }

    /* TM1 fallback — on-demand duty modulation when EIST is absent. */
    if (!g_cf.scale_ok && tm_cap) {
        g_cf.tm1 = 1;
        g_cf.scale_ok = 1;
        kprint("[CPUFREQ] no EIST — TM1 duty modulation fallback\n");
    }

    if (g_cf.scale_ok) {
        /* Resolve the live state so the first governor decision is a
         * real transition, not a no-op write to the running state. */
        uint32_t cur = (uint32_t)(rdmsr64(MSR_IA32_PERF_STATUS) & 0xFFFF);
        for (int i = 0; i < g_cf.n_states; i++)
            if (g_states[i].sts == cur) { g_cf.cur_idx = i; break; }
        kprint("[CPUFREQ] governor on — perf_status=");
        kprinthex(cur);
        kprint(" idx="); kprintdec((uint32_t)(g_cf.cur_idx < 0 ? 0 : g_cf.cur_idx));
        kprint("\n");
    } else {
        kprint("[CPUFREQ] inactive\n");
    }
}
