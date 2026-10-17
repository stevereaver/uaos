/* acpi_prt.c — ACPI _PRT routing-table resolver (UAOS-292)
 *
 * No AML interpreter — a byte scanner in the style of the cpufreq _PSS
 * probe.  Per table (DSDT + SSDTs) it collects Device/Scope container
 * ranges, finds each _PRT owner, decodes its package (direct
 * Name(_PRT, Pkg) or Method(_PRT) returning named/inline packages),
 * resolves link-device _CRS ExtendedIRQ descriptors to GSIs, and keys
 * the result by the bus the _PRT routes.  Devices behind bridges with
 * no _PRT of their own resolve by the standard PCI pin swizzle walked
 * up the bridge chain.
 *
 * q35 exercises the hard path: \_SB.PCI0._PRT is a Method selecting
 * PRTP (PIC links LNKA-H) or PRTA (APIC links GSIA-H) on PICF — the
 * GSIx links carry the real GSI in a Name(_CRS, Buffer) ExtendedIRQ
 * descriptor.  The LNKx _CRS is a Method returning the ISA IRQ, which
 * is exactly how the PIC candidate is told apart from the APIC one.
 */

#include "acpi_prt.h"
#include "irq.h"
#include "acpi.h"
#include "../klog/klog.h"
#include "../boot/kprint.h"
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Tiny AML readers                                                    */
/* ------------------------------------------------------------------ */

static int seg_ok(const uint8_t *s)
{
    for (int i = 0; i < 4; i++) {
        uint8_t c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

/* PkgLength at p.  Returns the byte count (spans the first length byte
 * through the end of the object) and the field width in *used. */
static uint32_t aml_pkglen(const uint8_t *t, uint32_t len, uint32_t p,
                           uint32_t *used)
{
    if (p >= len) return 0;
    uint8_t lead = t[p], n = (uint8_t)(lead >> 6);
    if (!n) { *used = 1; return lead & 0x3F; }
    if (p + 1 + n > len) return 0;
    uint32_t v = lead & 0x0F;
    for (uint8_t i = 0; i < n; i++) v |= (uint32_t)t[p + 1 + i] << (4 + 8 * i);
    *used = 1 + n;
    return v;
}

/* AML integer term: ZeroOp/OneOp/OnesOp/Byte/Word/DWord/QWord(low32).
 * Returns bytes consumed, 0 on non-integer. */
static uint32_t aml_int(const uint8_t *t, uint32_t len, uint32_t p,
                        uint32_t *v)
{
    if (p >= len) return 0;
    switch (t[p]) {
    case 0x00: *v = 0; return 1;
    case 0x01: *v = 1; return 1;
    case 0xFF: *v = 0xFFFFFFFFu; return 1;
    case 0x0A: if (p + 1 < len) { *v = t[p + 1]; return 2; } return 0;
    case 0x0B: if (p + 2 < len) {
            *v = (uint32_t)t[p + 1] | ((uint32_t)t[p + 2] << 8); return 3; }
        return 0;
    case 0x0C: if (p + 4 < len) {
            *v = (uint32_t)t[p + 1] | ((uint32_t)t[p + 2] << 8) |
                 ((uint32_t)t[p + 3] << 16) | ((uint32_t)t[p + 4] << 24);
            return 5; } return 0;
    case 0x0E: if (p + 8 < len) {
            *v = (uint32_t)t[p + 1] | ((uint32_t)t[p + 2] << 8) |
                 ((uint32_t)t[p + 3] << 16) | ((uint32_t)t[p + 4] << 24);
            return 9; } return 0;
    }
    return 0;
}

/* NameString at p: skips '^'/'\' prefixes, handles DualName (0x2E),
 * MultiName (0x2F), NullName (0x30) and bare NameSeg.  Copies the last
 * (leaf) nameseg to leaf[4].  Returns offset past the string or 0. */
static uint32_t aml_namestr(const uint8_t *t, uint32_t len, uint32_t p,
                            uint8_t leaf[4])
{
    while (p < len && (t[p] == 0x5E || t[p] == 0x5C)) p++;
    if (p >= len) return 0;
    if (t[p] == 0x2E) {
        if (p + 9 > len || !seg_ok(t + p + 5)) return 0;
        for (int i = 0; i < 4; i++) leaf[i] = t[p + 5 + i];
        return p + 9;
    }
    if (t[p] == 0x2F) {
        uint32_t cnt = t[p + 1];
        uint32_t e = p + 2 + 4 * cnt;
        if (!cnt || e > len || !seg_ok(t + e - 4)) return 0;
        for (int i = 0; i < 4; i++) leaf[i] = t[e - 4 + i];
        return e;
    }
    if (t[p] == 0x30) { leaf[0] = leaf[1] = leaf[2] = leaf[3] = 0; return p + 1; }
    if (p + 4 > len || !seg_ok(t + p)) return 0;
    for (int i = 0; i < 4; i++) leaf[i] = t[p + i];
    return p + 4;
}

static int seg_eq4(const uint8_t *p, const char *nm)
{
    return p[0] == (uint8_t)nm[0] && p[1] == (uint8_t)nm[1] &&
           p[2] == (uint8_t)nm[2] && p[3] == (uint8_t)nm[3];
}

/* ------------------------------------------------------------------ */
/* Container model — Devices and Scopes with byte ranges               */
/* ------------------------------------------------------------------ */

#define PRT_MAX_CONT 96

typedef struct {
    const uint8_t *t;          /* AML stream (after the SDT header)    */
    uint32_t len;
    uint32_t obj;              /* opcode offset                        */
    uint32_t start, end;       /* body range                           */
    uint8_t  name[4];          /* leaf nameseg                         */
    uint8_t  is_dev;
    int      parent;           /* enclosing container index, -1        */
    /* extracted direct children */
    uint32_t adr, bbn;
    uint8_t  has_adr, has_bbn, has_prt;
    uint32_t prt_pkg;          /* PackageOp offset for Name(_PRT,Pkg)  */
    uint32_t prt_ms, prt_me;   /* Method(_PRT) body range (0 if none)  */
} cont_t;

static cont_t  g_cont[PRT_MAX_CONT];
static int     g_ncont;

#define PRT_MAX_SETS 8
#define PRT_EMAX     128

typedef struct {
    uint8_t  bus;                    /* bus this _PRT routes            */
    uint8_t  has_bridge;             /* owned by a bridge function      */
    uint8_t  b_bus, b_dev, b_fn;     /* owning bridge BDF               */
    uint8_t  n;
    uint8_t  dev[PRT_EMAX];
    uint8_t  pin[PRT_EMAX];          /* 0-3                             */
    uint16_t gsi[PRT_EMAX];
} prt_set_t;

static prt_set_t g_sets[PRT_MAX_SETS];
static int       g_nsets;

#define PRT_MAX_BRIDGES 32
typedef struct { uint8_t bus, dev, fn, sec; } prt_bridge_t;
static prt_bridge_t g_br[PRT_MAX_BRIDGES];
static int          g_nbr;

static int g_inited;

/* Collect every Device (5B 82) and Scope (10) object in a table.
 * Candidates whose length or nameseg fail validation are skipped —
 * that is also what keeps bytes inside Buffers from matching. */
static void collect_containers(const uint8_t *t, uint32_t len)
{
    for (uint32_t i = 0; i + 7 < len; i++) {
        uint32_t pl, u, end;
        uint8_t leaf[4];
        if (t[i] == 0x5B && t[i + 1] == 0x82) {
            pl = aml_pkglen(t, len, i + 2, &u);
            end = i + 2 + pl;
            if (end <= i + 7 || end > len) continue;
            uint32_t ns = aml_namestr(t, len, i + 2 + u, leaf);
            if (!ns) continue;
            if (g_ncont >= PRT_MAX_CONT) return;
            cont_t *c = &g_cont[g_ncont++];
            for (int k = 0; k < (int)sizeof(*c); k++) ((uint8_t *)c)[k] = 0;
            c->t = t; c->len = len; c->obj = i;
            c->start = ns; c->end = end; c->is_dev = 1; c->parent = -1;
            for (int k = 0; k < 4; k++) c->name[k] = leaf[k];
        } else if (t[i] == 0x10) {
            pl = aml_pkglen(t, len, i + 1, &u);
            end = i + 1 + pl;
            if (end <= i + 6 || end > len) continue;
            uint32_t ns = aml_namestr(t, len, i + 1 + u, leaf);
            if (!ns) continue;
            if (g_ncont >= PRT_MAX_CONT) return;
            cont_t *c = &g_cont[g_ncont++];
            for (int k = 0; k < (int)sizeof(*c); k++) ((uint8_t *)c)[k] = 0;
            c->t = t; c->len = len; c->obj = i;
            c->start = ns; c->end = end; c->is_dev = 0; c->parent = -1;
            for (int k = 0; k < 4; k++) c->name[k] = leaf[k];
        }
    }
}

/* Innermost enclosing container for each container (same table only). */
static void assign_parents(int base)
{
    for (int j = base; j < g_ncont; j++) {
        cont_t *c = &g_cont[j];
        int best = -1;
        for (int k = base; k < g_ncont; k++) {
            if (k == j) continue;
            cont_t *o = &g_cont[k];
            if (o->t != c->t) continue;
            if (o->start <= c->obj && o->end >= c->end) {
                if (best < 0 ||
                    (o->end - o->start) < (g_cont[best].end - g_cont[best].start))
                    best = k;
            }
        }
        c->parent = best;
    }
}

/* The device a container contributes namespace for: a Device is its own
 * provider; a Scope resolves its leaf name to a Device in the same
 * table (transparent Scopes like \_SB fall through to their parent). */
static int provider_of(int ci)
{
    cont_t *c = &g_cont[ci];
    if (c->is_dev) return ci;
    for (int k = 0; k < g_ncont; k++) {
        cont_t *o = &g_cont[k];
        if (o->t != c->t || !o->is_dev) continue;
        if (o->name[0] == c->name[0] && o->name[1] == c->name[1] &&
            o->name[2] == c->name[2] && o->name[3] == c->name[3])
            return k;
    }
    if (c->parent >= 0) return provider_of(c->parent);
    return -1;
}

/* Ranges in which a device's direct children may appear: its own body
 * plus any Scope whose provider is this device. */
static int dev_regions(int di, uint32_t r[][2], int max)
{
    cont_t *d = &g_cont[di];
    int n = 0;
    if (n < max) { r[n][0] = d->start; r[n][1] = d->end; n++; }
    for (int k = 0; k < g_ncont && n < max; k++) {
        cont_t *s = &g_cont[k];
        if (s->t != d->t || s->is_dev) continue;
        if (provider_of(k) == di) { r[n][0] = s->start; r[n][1] = s->end; n++; }
    }
    return n;
}

/* Scan a region for Name(nm,obj)/Method(nm) as direct children —
 * bytes inside nested containers are skipped so a child's _ADR or _PRT
 * is never mistaken for the parent's.  For a Name the callback gets the
 * offset of the data object and the region end; for a Method the offset
 * of the flags byte and the method end. */
typedef void (*prt_op_cb)(const uint8_t *t, uint32_t end, uint32_t pos,
                          int is_method, void *arg);

static void scan_named(int di, uint32_t rs, uint32_t re, const char *nm,
                       prt_op_cb cb, void *arg)
{
    cont_t *d = &g_cont[di];
    const uint8_t *t = d->t;
    uint32_t i = rs;
    while (i + 5 <= re) {
        int skipped = 0;
        for (int k = 0; k < g_ncont; k++) {
            cont_t *o = &g_cont[k];
            if (o->t != t || o->obj < rs || o->end > re || o == d) continue;
            if (o->obj <= i && i < o->end) { i = o->end; skipped = 1; break; }
        }
        if (skipped) continue;
        if (t[i] == 0x08 && seg_eq4(t + i + 1, nm)) {
            cb(t, re, i + 5, 0, arg);
            i += 5;
            continue;
        }
        if (t[i] == 0x14) {
            uint32_t u;
            uint32_t pl = aml_pkglen(t, re, i + 1, &u);
            if (pl && i + 1 + pl <= re && i + 5 + u <= re &&
                seg_eq4(t + i + 1 + u, nm)) {
                cb(t, i + 1 + pl, i + 5 + u, 1, arg);
                i = i + 1 + pl;
                continue;
            }
        }
        i++;
    }
}

/* ------------------------------------------------------------------ */
/* _ADR / _BBN / _PRT extraction                                       */
/* ------------------------------------------------------------------ */

static void cb_adr(const uint8_t *t, uint32_t end, uint32_t pos,
                   int is_method, void *arg)
{
    (void)t; (void)end;
    if (is_method) return;
    cont_t *d = (cont_t *)arg;
    uint32_t v;
    if (aml_int(d->t, d->len, pos, &v)) { d->has_adr = 1; d->adr = v; }
}
static void cb_bbn(const uint8_t *t, uint32_t end, uint32_t pos,
                   int is_method, void *arg)
{
    (void)t; (void)end;
    if (is_method) return;
    cont_t *d = (cont_t *)arg;
    uint32_t v;
    if (aml_int(d->t, d->len, pos, &v)) { d->has_bbn = 1; d->bbn = v; }
}
static void cb_prt(const uint8_t *t, uint32_t end, uint32_t pos,
                   int is_method, void *arg)
{
    cont_t *d = (cont_t *)arg;
    if (is_method) {
        if (pos + 1 <= end) { d->has_prt = 1; d->prt_ms = pos + 1; d->prt_me = end; }
        return;
    }
    if (pos < end && t[pos] == 0x12) { d->has_prt = 1; d->prt_pkg = pos; }
}

/* ------------------------------------------------------------------ */
/* Resource buffers / link devices                                     */
/* ------------------------------------------------------------------ */

/* First ExtendedIRQ (large item 0x89) interrupt number in a resource
 * buffer, or -1. */
static int buf_extirq(const uint8_t *buf, uint32_t blen)
{
    for (uint32_t k = 0; k + 8 <= blen; k++) {
        if (buf[k] != 0x89) continue;
        uint32_t l = (uint32_t)buf[k + 1] | ((uint32_t)buf[k + 2] << 8);
        if (k + 3 + l > blen || l < 6) break;
        uint32_t cnt = buf[k + 4];
        if (!cnt || 2 + 4 * cnt > l) break;
        return (int)((uint32_t)buf[k + 5] | ((uint32_t)buf[k + 6] << 8) |
                     ((uint32_t)buf[k + 7] << 16) | ((uint32_t)buf[k + 8] << 24));
    }
    return -1;
}

/* `Name(leaf, BufferOp …)` anywhere in the table: returns the buffer
 * data length and sets *doff to the data offset, 0 on miss. */
static uint32_t find_named_buf(const uint8_t *t, uint32_t len,
                               const uint8_t leaf[4], uint32_t *doff)
{
    for (uint32_t i = 0; i + 7 < len; i++) {
        if (t[i] != 0x08 || !seg_eq4(t + i + 1, (const char *)leaf) ||
            t[i + 5] != 0x11)
            continue;
        uint32_t u;
        uint32_t bl = aml_pkglen(t, len, i + 6, &u);
        if (!bl || i + 6 + bl > len) continue;
        uint32_t base = i + 6 + u, sz, su = aml_int(t, len, base, &sz);
        if (!su || base + su >= i + 6 + bl) continue;
        *doff = base + su;
        return i + 6 + bl - (base + su);
    }
    return 0;
}

typedef struct { int gsi; const uint8_t *t; } crs_ctx_t;

static void cb_crs(const uint8_t *t, uint32_t end, uint32_t pos,
                   int is_method, void *arg)
{
    crs_ctx_t *cx = (crs_ctx_t *)arg;
    if (cx->gsi >= 0) return;
    if (is_method) {
        /* Method(_CRS){ Return(<named buffer>) } — chase the named
         * Buffer through the whole table (PIC-mode LNKx look like this;
         * whatever they yield is still fed through buf_extirq). */
        for (uint32_t q = pos + 1; q + 1 < end; q++) {
            if (t[q] != 0xA4) continue;
            if (t[q + 1] == 0x11) {                 /* inline buffer */
                uint32_t u;
                uint32_t bl = aml_pkglen(t, end, q + 2, &u);
                if (!bl || q + 2 + bl > end) continue;
                uint32_t sz, su = aml_int(t, end, q + 2 + u, &sz);
                if (!su) continue;
                cx->gsi = buf_extirq(t + q + 2 + u + su, bl - u - su);
            } else {
                uint8_t leaf[4];
                if (!aml_namestr(t, end, q + 1, leaf)) continue;
                uint32_t doff;
                uint32_t dlen = find_named_buf(t, end, leaf, &doff);
                if (dlen) cx->gsi = buf_extirq(t + doff, dlen);
            }
            return;
        }
        return;
    }
    if (pos >= end || t[pos] != 0x11) return;       /* BufferOp */
    uint32_t u;
    uint32_t bl = aml_pkglen(t, end, pos + 1, &u);
    if (!bl || pos + 1 + bl > end) return;
    uint32_t base = pos + 1 + u;
    uint32_t sz, su = aml_int(t, end, base, &sz);
    if (!su) return;
    uint32_t data = base + su;
    if (data >= pos + 1 + bl) return;
    cx->gsi = buf_extirq(t + data, pos + 1 + bl - data);
}

/* Resolve a link device by leaf name -> first ExtendedIRQ in its _CRS. */
static int link_gsi(cont_t *scope_hint, const uint8_t leaf[4])
{
    for (int k = 0; k < g_ncont; k++) {
        cont_t *d = &g_cont[k];
        if (d->t != scope_hint->t || !d->is_dev) continue;
        if (!(d->name[0] == leaf[0] && d->name[1] == leaf[1] &&
              d->name[2] == leaf[2] && d->name[3] == leaf[3]))
            continue;
        crs_ctx_t cx; cx.gsi = -1; cx.t = d->t;
        uint32_t regs[8][2];
        int nr = dev_regions(k, regs, 8);
        for (int r = 0; r < nr && cx.gsi < 0; r++)
            scan_named(k, regs[r][0], regs[r][1], "_CRS", cb_crs, &cx);
        if (cx.gsi >= 0) return cx.gsi;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* _PRT package decode                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *t; uint32_t len;
    cont_t *owner;
    int n, total, resolved, maxgsi;
    uint8_t  dev[PRT_EMAX], pin[PRT_EMAX];
    uint16_t gsi[PRT_EMAX];
} prt_dec_t;

static void prt_decode_pkg(prt_dec_t *out, uint32_t poff)
{
    const uint8_t *t = out->t;
    uint32_t len = out->len;
    out->n = out->total = out->resolved = out->maxgsi = 0;
    if (poff >= len || t[poff] != 0x12) return;
    uint32_t u;
    uint32_t pl = aml_pkglen(t, len, poff + 1, &u);
    if (!pl || poff + 1 + pl > len || poff + 1 + u >= len) return;
    uint32_t end = poff + 1 + pl;
    uint32_t cnt = t[poff + 1 + u];
    uint32_t q = poff + 2 + u;
    while (q < end && (uint32_t)out->total < cnt) {
        if (t[q] != 0x12) break;
        uint32_t u2;
        uint32_t pl2 = aml_pkglen(t, len, q + 1, &u2);
        if (!pl2 || q + 1 + pl2 > end || q + 1 + u2 >= end) break;
        uint32_t e = q + 1 + pl2, r = q + 1 + u2 + 1;
        uint32_t addr, pin, idx, w;
        w = aml_int(t, len, r, &addr);
        if (!w) break;
        r += w;
        w = aml_int(t, len, r, &pin);
        if (!w) break;
        r += w;
        if (r >= len) break;
        int gsi = -1;
        if (t[r] == 0x00) {                       /* ZeroOp: idx = GSI */
            r += 1;
            w = aml_int(t, len, r, &idx);
            if (!w) break;
            r += w;
            gsi = (int)idx;
        } else {                                  /* link device name */
            uint8_t leaf[4];
            uint32_t ne = aml_namestr(t, len, r, leaf);
            if (!ne) break;
            r = ne;
            w = aml_int(t, len, r, &idx);
            if (!w) break;
            r += w;
            gsi = link_gsi(out->owner, leaf);
        }
        out->total++;
        if (pin <= 3 && gsi >= 0 && out->n < PRT_EMAX) {
            out->dev[out->n] = (uint8_t)((addr >> 16) & 0x1F);
            out->pin[out->n] = (uint8_t)pin;
            out->gsi[out->n] = (uint16_t)gsi;
            out->resolved++;
            if (gsi > out->maxgsi) out->maxgsi = gsi;
            out->n++;
        }
        q = e;
    }
}

/* Strongly prefer fully-decoded packages and APIC-looking results —
 * a PIC (ISA) candidate never yields a source >= 16. */
static int prt_score(const prt_dec_t *d)
{
    if (!d->resolved) return 0;
    int s = d->resolved;
    if (d->n > 0 && d->resolved == d->total) s += 500;
    if (d->maxgsi >= 16) s += 300;
    return s;
}

/* `Name(leaf, Package)` — search the owner's regions first, then the
 * whole table (covers enclosing scopes and top-level names). */
static uint32_t find_named_pkg(int di, const uint8_t leaf[4])
{
    cont_t *d = &g_cont[di];
    char nm[5] = { (char)leaf[0], (char)leaf[1], (char)leaf[2],
                   (char)leaf[3], 0 };
    uint32_t regs[8][2];
    int nr = dev_regions(di, regs, 8);
    for (int r = 0; r < nr; r++) {
        const uint8_t *t = d->t;
        uint32_t i = regs[r][0];
        while (i + 6 <= regs[r][1]) {
            if (t[i] == 0x08 && seg_eq4(t + i + 1, nm) && t[i + 5] == 0x12)
                return i + 5;
            i++;
        }
    }
    const uint8_t *t = d->t;
    for (uint32_t i = 0; i + 7 < d->len; i++)
        if (t[i] == 0x08 && seg_eq4(t + i + 1, (const char *)leaf) &&
            t[i + 5] == 0x12)
            return i + 5;
    return 0;
}

/* Decode a device's _PRT — the best-scoring candidate wins. */
static void prt_decode_owner(int di, prt_dec_t *out)
{
    cont_t *d = &g_cont[di];
    prt_dec_t best;
    best.t = d->t; best.len = d->len; best.owner = d;
    best.n = best.total = best.resolved = best.maxgsi = 0;
    int best_s = 0;

    if (d->prt_pkg) {
        prt_dec_t c; c.t = d->t; c.len = d->len; c.owner = d;
        prt_decode_pkg(&c, d->prt_pkg);
        int s = prt_score(&c);
        if (s > best_s) { best_s = s; best = c; }
    }
    if (d->prt_ms) {
        const uint8_t *t = d->t;
        for (uint32_t q = d->prt_ms; q < d->prt_me; q++) {
            if (t[q] != 0xA4) continue;
            uint32_t tgt = q + 1;
            if (tgt >= d->prt_me) break;
            uint32_t poff = 0;
            if (t[tgt] == 0x12) poff = tgt;
            else {
                uint8_t leaf[4];
                if (aml_namestr(t, d->prt_me, tgt, leaf))
                    poff = find_named_pkg(di, leaf);
            }
            if (!poff) continue;
            prt_dec_t c; c.t = d->t; c.len = d->len; c.owner = d;
            prt_decode_pkg(&c, poff);
            int s = prt_score(&c);
            if (s > best_s) { best_s = s; best = c; }
        }
    }
    *out = best;
}

/* ------------------------------------------------------------------ */
/* Topology                                                            */
/* ------------------------------------------------------------------ */

/* Nearest ancestor DEVICE of ci (scopes resolve to their target). */
static int anc_dev(int ci)
{
    int p = g_cont[ci].parent;
    while (p >= 0) {
        int dv = provider_of(p);
        if (dv >= 0) return dv;
        p = g_cont[p].parent;
    }
    return -1;
}

static int has_adr_ancestor(int ci, int depth)
{
    if (depth > 8) return 0;
    int a = anc_dev(ci);
    if (a < 0) return 0;
    if (g_cont[a].has_adr) return 1;
    return has_adr_ancestor(a, depth + 1);
}

static int is_pci_bridge(uint8_t bus, uint8_t dev, uint8_t fn)
{
    if (IRQ_PciRead32(bus, dev, fn, 0x00) == 0xFFFFFFFF) return 0;
    return (IRQ_PciRead8(bus, dev, fn, 0x0E) & 0x7F) == 1;
}

/* Bus number a device sits on: an _ADR ancestor that itself has an _ADR
 * ancestor is a real PCI-PCI bridge (our device lives on its secondary
 * bus); an ancestor with no _ADR ancestry is the host bridge and our
 * device sits on its routed bus. */
static int bus_on(int ci, int depth)
{
    if (depth > 8) return -1;
    int a = anc_dev(ci);
    if (a < 0) return 0;
    cont_t *A = &g_cont[a];
    if (A->has_adr && has_adr_ancestor(a, depth + 1)) {
        uint8_t ad = (uint8_t)(A->adr >> 16), af = (uint8_t)(A->adr & 0x1F);
        int pb = bus_on(a, depth + 1);
        if (pb < 0 || !is_pci_bridge((uint8_t)pb, ad, af)) return -1;
        uint8_t sec = IRQ_PciRead8((uint8_t)pb, ad, af, 0x19);
        if (!sec || sec == 0xFF || sec == pb) return -1;
        return sec;
    }
    return A->has_bbn ? (int)A->bbn : 0;
}

/* ------------------------------------------------------------------ */
/* PCI bridge scan for the swizzle walk                                */
/* ------------------------------------------------------------------ */

static void pci_scan_bus(uint8_t bus, int depth)
{
    if (depth > 6 || g_nbr >= PRT_MAX_BRIDGES) return;
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint8_t fns = 1;
        for (uint8_t fn = 0; fn < fns; fn++) {
            uint32_t id = IRQ_PciRead32(bus, dev, fn, 0x00);
            if (id == 0xFFFFFFFF || id == 0) { if (!fn) break; continue; }
            if (!fn && (IRQ_PciRead8(bus, dev, 0, 0x0E) & 0x80)) fns = 8;
            if ((IRQ_PciRead8(bus, dev, fn, 0x0E) & 0x7F) != 1) continue;
            uint8_t sec = IRQ_PciRead8(bus, dev, fn, 0x19);
            if (!sec || sec == 0xFF || sec == bus) continue;
            if (g_nbr >= PRT_MAX_BRIDGES) return;
            g_br[g_nbr].bus = bus; g_br[g_nbr].dev = dev;
            g_br[g_nbr].fn = fn;   g_br[g_nbr].sec = sec;
            g_nbr++;
            pci_scan_bus(sec, depth + 1);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

static void scan_table(const uint8_t *aml, uint32_t len)
{
    int base = g_ncont;
    collect_containers(aml, len);
    assign_parents(base);

    for (int di = base; di < g_ncont; di++) {
        cont_t *d = &g_cont[di];
        if (!d->is_dev) continue;
        uint32_t regs[8][2];
        int nr = dev_regions(di, regs, 8);
        for (int r = 0; r < nr; r++) {
            scan_named(di, regs[r][0], regs[r][1], "_ADR", cb_adr, d);
            scan_named(di, regs[r][0], regs[r][1], "_BBN", cb_bbn, d);
            scan_named(di, regs[r][0], regs[r][1], "_PRT", cb_prt, d);
        }
    }

    for (int di = base; di < g_ncont; di++) {
        cont_t *d = &g_cont[di];
        if (!d->is_dev || !d->has_prt) continue;

        prt_dec_t out;
        prt_decode_owner(di, &out);
        if (!out.n) {
            KLOG(KLOG_KERN, KLOG_DEBUG,
                 "acpi-prt: %.4s _PRT present, no APIC-decodable package\n",
                 (const char *)d->name);
            continue;
        }

        uint8_t bus, has_b = 0, bb = 0, bd = 0, bf = 0;
        if (!has_adr_ancestor(di, 0)) {
            bus = d->has_bbn ? (uint8_t)d->bbn : 0;      /* host bridge */
        } else if (d->has_adr) {
            uint8_t dd = (uint8_t)(d->adr >> 16), df = (uint8_t)(d->adr & 0x1F);
            int pb = bus_on(di, 0);
            if (pb < 0 || pb > 255 || !is_pci_bridge((uint8_t)pb, dd, df)) {
                KLOG(KLOG_KERN, KLOG_DEBUG,
                     "acpi-prt: %.4s _PRT but owner is no PCI bridge\n",
                     (const char *)d->name);
                continue;
            }
            uint8_t sec = IRQ_PciRead8((uint8_t)pb, dd, df, 0x19);
            if (!sec || sec == 0xFF || sec == pb) continue;
            bus = sec; has_b = 1; bb = (uint8_t)pb; bd = dd; bf = df;
        } else {
            continue;   /* nested _PRT owner without _ADR — can't key it */
        }

        if (g_nsets >= PRT_MAX_SETS) break;
        prt_set_t *s = &g_sets[g_nsets];
        s->bus = bus; s->has_bridge = has_b;
        s->b_bus = bb; s->b_dev = bd; s->b_fn = bf;
        s->n = (uint8_t)out.n;
        for (int e = 0; e < out.n; e++) {
            s->dev[e] = out.dev[e]; s->pin[e] = out.pin[e];
            s->gsi[e] = out.gsi[e];
        }
        g_nsets++;
        KLOG(KLOG_KERN, KLOG_TRACE,
             "acpi-prt: %s %u:%u.%u routes bus %u, %u entries\n",
             has_b ? "bridge" : "host  ", bb, bd, bf,
             (uint32_t)bus, (uint32_t)out.n);
    }
}

void AcpiPrt_Init(void)
{
    if (g_inited) return;
    g_inited = 1;

    const uint8_t *dsdt = (const uint8_t *)ACPI_Dsdt();
    if (dsdt) {
        uint32_t len = *(const uint32_t *)(dsdt + 4);   /* SDT length */
        if (len > 36) scan_table(dsdt + 36, len - 36);
    }
    for (int i = 0; i < 8; i++) {
        const uint8_t *ssdt = (const uint8_t *)ACPI_FindTableN("SSDT", i);
        if (!ssdt) break;
        uint32_t len = *(const uint32_t *)(ssdt + 4);
        if (len > 36) scan_table(ssdt + 36, len - 36);
    }

    pci_scan_bus(0, 0);

    int total = 0;
    for (int i = 0; i < g_nsets; i++) total += g_sets[i].n;
    KLOG(KLOG_KERN, KLOG_DEBUG, "acpi-prt: %u sets, %u entries\n",
         (uint32_t)g_nsets, (uint32_t)total);
}

/* ------------------------------------------------------------------ */
/* Resolver                                                            */
/* ------------------------------------------------------------------ */

static uint8_t g_last_bb, g_last_bd, g_last_bf, g_last_swz;

static int prt_match(uint8_t bus, uint8_t dev, uint8_t pin)
{
    for (int s = 0; s < g_nsets; s++) {
        if (g_sets[s].bus != bus) continue;
        for (uint8_t e = 0; e < g_sets[s].n; e++)
            if (g_sets[s].dev[e] == dev && g_sets[s].pin[e] == pin - 1)
                return g_sets[s].gsi[e];
    }
    return -1;
}

int AcpiPrt_Route(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t pin,
                  int *via_swizzle)
{
    (void)fn;   /* _PRT entries key (dev,pin) — fn is wildcarded */
    g_last_swz = 0;
    if (pin < 1 || pin > 4) return -1;
    for (int hop = 0; hop < 8; hop++) {
        int gsi = prt_match(bus, dev, pin);
        if (gsi >= 0) {
            if (via_swizzle) *via_swizzle = g_last_swz;
            return gsi;
        }
        if (!bus) return -1;                    /* bus 0 has no parent */
        int bi = -1;
        for (int k = 0; k < g_nbr; k++)
            if (g_br[k].sec == bus) { bi = k; break; }
        if (bi < 0) return -1;
        g_last_bb = g_br[bi].bus; g_last_bd = g_br[bi].dev;
        g_last_bf = g_br[bi].fn;  g_last_swz = 1;
        /* PCI INTx swizzle: the pin arriving at the bridge is rotated
         * by the child slot's device number. */
        pin = (uint8_t)(((pin - 1 + dev) & 3) + 1);
        bus = g_br[bi].bus;
        dev = g_br[bi].dev;
        fn  = g_br[bi].fn;
    }
    return -1;
}

int AcpiPrt_LastBridge(uint8_t *bus, uint8_t *dev, uint8_t *fn)
{
    if (!g_last_swz) return -1;
    *bus = g_last_bb; *dev = g_last_bd; *fn = g_last_bf;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Dump accessors                                                      */
/* ------------------------------------------------------------------ */

int AcpiPrt_NumSets(void) { return g_nsets; }

int AcpiPrt_SetInfo(int set, uint8_t *bus, int *has_bridge,
                    uint8_t *bbus, uint8_t *bdev, uint8_t *bfn)
{
    if (set < 0 || set >= g_nsets) return -1;
    *bus = g_sets[set].bus;
    *has_bridge = g_sets[set].has_bridge;
    *bbus = g_sets[set].b_bus; *bdev = g_sets[set].b_dev;
    *bfn  = g_sets[set].b_fn;
    return 0;
}

int AcpiPrt_SetNEntries(int set)
{
    return (set >= 0 && set < g_nsets) ? g_sets[set].n : 0;
}

int AcpiPrt_SetEntry(int set, int idx, uint8_t *dev, uint8_t *pin, int *gsi)
{
    if (set < 0 || set >= g_nsets || idx < 0 || idx >= g_sets[set].n)
        return -1;
    *dev = g_sets[set].dev[idx];
    *pin = g_sets[set].pin[idx] + 1;
    *gsi = g_sets[set].gsi[idx];
    return 0;
}
