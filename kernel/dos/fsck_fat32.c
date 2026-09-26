/*
 * fsck_fat32.c — FAT32 filesystem checker for the UAOS fsck framework
 *
 * Independent re-implementation of FAT32 on-disk structures: deliberately
 * does NOT share state with the live driver (kernel/dos/fat32.c) so a check
 * can run against a mounted volume without corrupting the handler's cached
 * Fat32FS or its static buffers.
 *
 * Check phases:
 *   1. Boot sector / BPB validation (with backup-boot-sector recovery)
 *   2. FSINFO signature + counters
 *   3. FAT copy comparison (FAT1 vs FAT2)
 *   4. FAT entry scan: free/used/bad/invalid-link accounting
 *   5. Directory tree walk: entry sanity, chain marking, cross-link and
 *      loop detection, size-vs-chain-length reconciliation, dot entries
 *   6. Lost cluster detection + optional reclaim
 *   7. Free-space reconciliation + FSINFO repair
 *
 * Repairs write through BlockDev_Write and always update every FAT copy.
 */

#include "fsck.h"
#include "fat32.h"
#include <string.h>

/* =========================================================================
 * Limits and buffers
 * ========================================================================= */

/* 1-bit "referenced" map plus a 1-bit "lost" map — each covers up to
 * 4 Mi clusters (e.g. 16 GiB at 4 KiB clusters) in 512 KiB of BSS. */
#define FK_MAX_CLUSTERS   (4u * 1024u * 1024u)
static uint8_t g_ref[FK_MAX_CLUSTERS / 8];
static uint8_t g_lost[FK_MAX_CLUSTERS / 8];

static uint8_t g_sec[512]      __attribute__((aligned(4096)));
static uint8_t g_sec2[512]     __attribute__((aligned(4096)));
static uint8_t g_fatc[512]     __attribute__((aligned(4096))); /* FAT cache    */
static uint8_t g_scan[64*1024] __attribute__((aligned(4096))); /* FAT/dir data */
static uint8_t g_dirw[64*1024] __attribute__((aligned(4096))); /* dir init write */

static uint32_t g_fatc_sec = 0xFFFFFFFF;   /* sector cached in g_fatc */

#define FAT32_EOC_MIN   0x0FFFFFF8u
#define FAT32_BAD       0x0FFFFFF7u
#define FK_IS_EOC(c)    ((c) >= FAT32_EOC_MIN)
#define FK_DIR_ENT      32

#define LINE_MAX 112

/* =========================================================================
 * Geometry
 * ========================================================================= */
typedef struct {
    uint32_t bps;            /* bytes per sector (must be 512 to check)   */
    uint32_t spc;            /* sectors per cluster                        */
    uint32_t clus_size;
    uint32_t rsvd;           /* reserved sectors                           */
    uint32_t nfats;
    uint32_t fat_sz;         /* sectors per FAT                            */
    uint32_t tot_sec;
    uint32_t fat_start;
    uint32_t data_start;
    uint32_t root_clus;
    uint32_t total_clusters; /* data clusters (2..total+1 valid)           */
    uint32_t fsinfo_sec;
    uint32_t bk_boot;
    int      over_cap;       /* tot_sec > device capacity                  */
} FkGeo;

/* =========================================================================
 * Small line helpers
 * ========================================================================= */
static int l_slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void l_cat(char *d, const char *s, int max)
{
    int dl = l_slen(d), i = 0;
    while (s[i] && dl + i < max - 1) { d[dl + i] = s[i]; i++; }
    d[dl + i] = 0;
}
static void l_num(char *d, uint64_t v, int max)
{
    char n[24]; fk_dec(v, n, sizeof(n)); l_cat(d, n, max);
}
static void l_hex(char *d, uint64_t v, int max)
{
    char n[24]; fk_hex(v, n, sizeof(n)); l_cat(d, n, max);
}

/* =========================================================================
 * Bitmap helpers
 * ========================================================================= */
static int  ref_get(uint32_t c)   { return (g_ref[c >> 3] >> (c & 7)) & 1; }
static void ref_set(uint32_t c)   { g_ref[c >> 3] |= (uint8_t)(1 << (c & 7)); }
static int  lost_get(uint32_t c)  { return (g_lost[c >> 3] >> (c & 7)) & 1; }
static void lost_set(uint32_t c)  { g_lost[c >> 3] |= (uint8_t)(1 << (c & 7)); }

/* =========================================================================
 * FAT I/O
 * ========================================================================= */
static uint32_t fat_sector_of(FkGeo *g, uint32_t cluster)
{
    return g->fat_start + (cluster * 4) / g->bps;
}

/* Read one FAT entry through the 1-sector cache.  With 512-byte sectors a
 * 4-byte-aligned entry can never span a sector boundary. */
static uint32_t fat_get(BlockDev *dev, FkGeo *g, uint32_t cluster)
{
    uint32_t sec = fat_sector_of(g, cluster);
    uint32_t off = (cluster * 4) % g->bps;
    if (g_fatc_sec != sec) {
        int ok = 0;
        for (int a = 0; a < 3 && !ok; a++)
            ok = (BlockDev_Read(dev, sec, g_fatc, 1) == 0);
        if (!ok) {
            g_fatc_sec = 0xFFFFFFFF;
            return FAT32_EOC_MIN;   /* treat read error as chain end */
        }
        g_fatc_sec = sec;
    }
    return fk_le32(g_fatc + off) & 0x0FFFFFFF;
}

/* Write one FAT entry to every FAT copy. */
static int fat_set(BlockDev *dev, FkGeo *g, uint32_t cluster, uint32_t value)
{
    uint32_t rel = (cluster * 4) / g->bps;   /* sector index within one FAT */
    uint32_t off = (cluster * 4) % g->bps;
    int rc = 0;
    for (uint32_t f = 0; f < g->nfats; f++) {
        uint32_t sec = g->fat_start + f * g->fat_sz + rel;
        if (BlockDev_Read(dev, sec, g_sec, 1) != 0) { rc = -1; continue; }
        /* preserve the top (reserved) nibble of the original entry */
        uint32_t old = fk_le32(g_sec + off) & 0xF0000000;
        fk_put32(g_sec + off, old | (value & 0x0FFFFFFF));
        if (BlockDev_Write(dev, sec, g_sec, 1) != 0) rc = -1;
    }
    g_fatc_sec = 0xFFFFFFFF;
    return rc;
}

static uint32_t clus_to_sec(FkGeo *g, uint32_t c)
{
    return g->data_start + (c - 2) * g->spc;
}

/* Read with retries — the virtio path can transiently fail under sustained
 * I/O bursts, and a single hiccup should not abort a multi-minute scan. */
static int fk_rd(BlockDev *dev, uint64_t sec, void *buf, uint32_t n)
{
    for (int a = 0; a < 3; a++)
        if (BlockDev_Read(dev, sec, buf, n) == 0) return 0;
    return -1;
}

/* =========================================================================
 * Directory-entry fix helpers
 * ========================================================================= */

/* Patch `len` bytes of the 32-byte dir entry `e` inside directory cluster
 * `clus` — the entry lives in the cluster currently being scanned, which is
 * not necessarily the chain head. */
static int dirent_patch(BlockDev *dev, FkGeo *g,
                        uint32_t clus, uint32_t e,
                        uint32_t field_off, const uint8_t *data, int len)
{
    uint32_t sec  = clus_to_sec(g, clus) + (e * FK_DIR_ENT) / g->bps;
    uint32_t off  = (e * FK_DIR_ENT) % g->bps + field_off;
    if (BlockDev_Read(dev, sec, g_sec, 1) != 0) return -1;
    memcpy(g_sec + off, data, (size_t)len);
    if (BlockDev_Write(dev, sec, g_sec, 1) != 0) return -1;
    return 0;
}

static int dirent_set_cluster(BlockDev *dev, FkGeo *g,
                              uint32_t clus, uint32_t e,
                              uint32_t new_clus)
{
    uint8_t b[2];
    fk_put16(b, (uint16_t)(new_clus >> 16));
    if (dirent_patch(dev, g, clus, e, 20, b, 2)) return -1;
    fk_put16(b, (uint16_t)(new_clus & 0xFFFF));
    return dirent_patch(dev, g, clus, e, 26, b, 2);
}

static int dirent_set_size(BlockDev *dev, FkGeo *g,
                           uint32_t clus, uint32_t e,
                           uint32_t new_size)
{
    uint8_t b[4]; fk_put32(b, new_size);
    return dirent_patch(dev, g, clus, e, 28, b, 4);
}

/* =========================================================================
 * Phase 1 — BPB
 * ========================================================================= */
static int bpb_valid(const uint8_t *s)
{
    if (s[510] != 0x55 || s[511] != 0xAA) return 0;
    uint16_t bps = fk_le16(s + 11);
    uint8_t  spc = s[13];
    if (bps != 512) return 0;                 /* we check 512-byte BPB only */
    if (spc == 0 || (spc & (spc - 1)) || spc > 128) return 0;
    if (s[16] == 0) return 0;                 /* num_fats */
    if (fk_le16(s + 17) != 0) return 0;       /* root_ent_cnt must be 0     */
    if (fk_le16(s + 22) != 0) return 0;       /* fat_sz16 must be 0         */
    if (fk_le32(s + 36) == 0) return 0;       /* fat_sz32 required          */
    if (fk_le32(s + 44) < 2)  return 0;       /* root_clus >= 2             */
    return 1;
}

static void geo_from_bpb(FkGeo *g, const uint8_t *s, uint64_t cap)
{
    memset(g, 0, sizeof(*g));
    g->bps        = fk_le16(s + 11);
    g->spc        = s[13];
    g->clus_size  = g->bps * g->spc;
    g->rsvd       = fk_le16(s + 14);
    g->nfats      = s[16];
    g->tot_sec    = fk_le32(s + 32);
    if (!g->tot_sec) g->tot_sec = fk_le16(s + 19);
    g->fat_sz     = fk_le32(s + 36);
    g->root_clus  = fk_le32(s + 44);
    g->fsinfo_sec = fk_le16(s + 48);
    if (!g->fsinfo_sec) g->fsinfo_sec = 1;
    g->bk_boot    = fk_le16(s + 50);
    if (!g->bk_boot) g->bk_boot = 6;
    g->fat_start  = g->rsvd;
    g->data_start = g->rsvd + g->nfats * g->fat_sz;
    if (g->tot_sec > g->data_start)
        g->total_clusters = (g->tot_sec - g->data_start) / g->spc;
    /* Clamp to FAT capacity like the driver does */
    uint32_t fat_cap = g->fat_sz * (g->bps / 4);
    if (fat_cap > 2 && g->total_clusters > fat_cap - 2)
        g->total_clusters = fat_cap - 2;
    g->over_cap = (g->tot_sec > cap);
}

static int phase_bpb(BlockDev *dev, FkGeo *g, FsckCtx *ctx)
{
    char line[LINE_MAX];
    int src = 0;   /* 0 = primary, 6/elsewhere = backup */

    if (BlockDev_Read(dev, 0, g_sec, 1) != 0) {
        fsck_err(ctx, "cannot read boot sector");
        return -1;
    }
    memcpy(g_sec2, g_sec, 512);

    if (!bpb_valid(g_sec)) {
        /* Primary BPB bad — see if the conventional backup (sector 6) is
         * usable, and offer to restore it. */
        fsck_err(ctx, "boot sector is not a valid FAT32 BPB");
        if (BlockDev_Read(dev, 6, g_sec, 1) == 0 && bpb_valid(g_sec)) {
            fsck_note(ctx, "backup boot sector at sector 6 is valid");
            if (fsck_should_fix(ctx)) {
                if (BlockDev_Write(dev, 0, g_sec, 1) == 0) {
                    ctx->fixed++;
                    fsck_out(ctx, "  boot sector restored from backup");
                    src = 6;
                } else {
                    fsck_err(ctx, "boot sector restore write failed");
                }
            } else {
                return -1;   /* cannot proceed without a valid BPB */
            }
        } else {
            return -1;
        }
    }

    /* g_sec now holds the (possibly restored) BPB */
    geo_from_bpb(g, g_sec, dev->num_sectors);
    if (src) memcpy(g_sec2, g_sec, 512);
    (void)src;

    /* Compare against backup copy (informational) */
    if (g->bk_boot && g->bk_boot < g->rsvd &&
        BlockDev_Read(dev, g->bk_boot, g_sec, 1) == 0 &&
        memcmp(g_sec, g_sec2, 512) != 0) {
        fsck_warn(ctx, "backup boot sector differs from primary");
    }

    /* Soft field checks */
    if (fk_le16(g_sec2 + 38) != 0)
        fsck_warn(ctx, "non-zero FAT32 filesystem version (fs_ver)");
    if (!(g_sec2[21] == 0xF0 || g_sec2[21] >= 0xF8)) {
        fsck_warn(ctx, "unusual media descriptor byte");
        if (fsck_should_fix(ctx)) {
            g_sec2[21] = 0xF8;
            if (BlockDev_Write(dev, 0, g_sec2, 1) == 0) ctx->fixed++;
        }
    }
    if (g->rsvd < 2)
        fsck_err(ctx, "reserved sector count too small for FSINFO+backup");
    if (g->root_clus > g->total_clusters + 1) {
        fsck_err(ctx, "root cluster beyond data area");
        return -1;
    }
    if (g->over_cap) {
        line[0] = 0;
        l_cat(line, "BPB total sectors (", LINE_MAX);
        l_num(line, g->tot_sec, LINE_MAX);
        l_cat(line, ") exceeds device capacity (", LINE_MAX);
        l_num(line, dev->num_sectors, LINE_MAX);
        l_cat(line, ")", LINE_MAX);
        fsck_err(ctx, line);
    }
    if (g->total_clusters == 0) {
        fsck_err(ctx, "no usable data clusters");
        return -1;
    }
    if (g->total_clusters + 2 > FK_MAX_CLUSTERS) {
        line[0] = 0;
        l_cat(line, "volume too large to check (", LINE_MAX);
        l_num(line, g->total_clusters, LINE_MAX);
        l_cat(line, " clusters, max ", LINE_MAX);
        l_num(line, FK_MAX_CLUSTERS, LINE_MAX);
        l_cat(line, ")", LINE_MAX);
        fsck_err(ctx, line);
        return -1;
    }
    /* First FAT entries must look sane: [0]=0xFFFFFFxx, [1]=EOC */
    if (g->root_clus <= g->total_clusters + 1) {
        uint32_t e0 = fat_get(dev, g, 0);
        uint32_t e1 = fat_get(dev, g, 1);
        if ((e0 & 0x0FFFFF00) != 0x0FFFFF00)
            fsck_warn(ctx, "FAT entry 0 does not carry the media descriptor");
        if (!FK_IS_EOC(e1))
            fsck_warn(ctx, "FAT entry 1 is not the EOC marker");
    }
    return 0;
}

/* =========================================================================
 * Phase 2 — FSINFO
 * ========================================================================= */
typedef struct {
    int      present;
    int      sig_ok;
    uint32_t free_cnt;
    uint32_t next_free;
} FkFsInfo;

static int phase_fsinfo(BlockDev *dev, FkGeo *g, FkFsInfo *fi, FsckCtx *ctx)
{
    memset(fi, 0, sizeof(*fi));
    if (g->fsinfo_sec >= g->rsvd) return 0;   /* outside reserved area */
    if (BlockDev_Read(dev, g->fsinfo_sec, g_sec, 1) != 0) return 0;
    fi->present   = 1;
    fi->sig_ok    = (fk_le32(g_sec) == 0x41615252 &&
                     fk_le32(g_sec + 484) == 0x61417272 &&
                     fk_le32(g_sec + 508) == 0xAA550000);
    fi->free_cnt  = fk_le32(g_sec + 488);
    fi->next_free = fk_le32(g_sec + 492);
    if (!fi->sig_ok)
        fsck_err(ctx, "FSINFO signatures corrupted");
    if (fi->free_cnt != 0xFFFFFFFF && fi->free_cnt > g->total_clusters)
        fsck_err(ctx, "FSINFO free-cluster count out of range");
    if (fi->next_free != 0xFFFFFFFF &&
        (fi->next_free < 2 || fi->next_free > g->total_clusters + 1))
        fsck_err(ctx, "FSINFO next-free hint out of range");
    return 0;
}

/* Rewrite FSINFO with corrected counters (and repaired signatures). */
static int fsinfo_fix(BlockDev *dev, FkGeo *g, uint32_t free_cnt, FsckCtx *ctx)
{
    memset(g_sec, 0, 512);
    fk_put32(g_sec + 0,   0x41615252);
    fk_put32(g_sec + 484, 0x61417272);
    fk_put32(g_sec + 488, free_cnt);
    fk_put32(g_sec + 492, 2);             /* conservative next_free hint */
    fk_put32(g_sec + 508, 0xAA550000);
    return BlockDev_Write(dev, g->fsinfo_sec, g_sec, 1);
}

/* =========================================================================
 * Phase 3 — FAT copy comparison
 * ========================================================================= */
static int phase_fat_compare(BlockDev *dev, FkGeo *g, FsckCtx *ctx)
{
    if (g->nfats < 2) return 0;
    char line[LINE_MAX];
    uint32_t diffs = 0;

    for (uint32_t s = 0; s < g->fat_sz; s += 64) {
        uint32_t n = (g->fat_sz - s > 64) ? 64 : g->fat_sz - s;
        if (fk_rd(dev, g->fat_start + s, g_scan, n) != 0) {
            fsck_err(ctx, "cannot read FAT1 for comparison");
            return 0;
        }
        if (fk_rd(dev, g->fat_start + g->fat_sz + s, g_dirw, n) != 0) {
            fsck_err(ctx, "cannot read FAT2 for comparison");
            return 0;
        }
        for (uint32_t b = 0; b < n * 512; b += 4) {
            /* Compare full 32-bit entries including reserved nibble —
             * bit-exact is what the driver writes anyway. */
            if (memcmp(g_scan + b, g_dirw + b, 4) != 0) diffs++;
        }
        fsck_yield(ctx);
        if (fsck_break(ctx)) return -1;
    }

    if (diffs == 0) {
        fsck_note(ctx, "FAT copies identical");
        return 0;
    }

    line[0] = 0;
    l_cat(line, "FAT1 and FAT2 differ in ", LINE_MAX);
    l_num(line, diffs, LINE_MAX);
    l_cat(line, " entries", LINE_MAX);
    fsck_err(ctx, line);
    if (fsck_should_fix(ctx)) {
        /* Copy FAT1 over FAT2 in batches. */
        int ok = 1;
        for (uint32_t s = 0; s < g->fat_sz; s += 128) {
            uint32_t n = (g->fat_sz - s > 128) ? 128 : g->fat_sz - s;
            if (fk_rd(dev, g->fat_start + s, g_scan, n) != 0 ||
                BlockDev_Write(dev, g->fat_start + g->fat_sz + s,
                               g_scan, n) != 0) { ok = 0; break; }
            fsck_yield(ctx);
        }
        if (ok) { ctx->fixed++; fsck_out(ctx, "  FAT2 synchronised from FAT1"); }
        else    fsck_err(ctx, "FAT2 sync write failed");
    }
    return 0;
}

/* =========================================================================
 * Phase 4 — FAT entry scan
 * ========================================================================= */
typedef struct {
    uint32_t free_cnt;
    uint32_t used_cnt;
    uint32_t bad_cnt;
    uint32_t invalid_links;   /* entries pointing outside the data area */
} FkFatStat;

static int phase_fat_scan(BlockDev *dev, FkGeo *g, FkFatStat *st, FsckCtx *ctx)
{
    char line[LINE_MAX];
    memset(st, 0, sizeof(*st));
    memset(g_ref, 0, (g->total_clusters + 2 + 7) / 8);

    uint32_t shown = 0;
    for (uint32_t s = 0; s < g->fat_sz; s += 128) {
        uint32_t n = (g->fat_sz - s > 128) ? 128 : g->fat_sz - s;
        uint32_t sec = g->fat_start + s;
        if (fk_rd(dev, sec, g_scan, n) != 0) {
            fsck_err(ctx, "cannot read FAT — aborting scan");
            return -1;
        }
        uint32_t ents = n * (g->bps / 4);
        for (uint32_t i = 0; i < ents; i++) {
            uint32_t c = s * (g->bps / 4) + i;
            if (c < 2) continue;
            if (c > g->total_clusters + 1) break;
            uint32_t v = fk_le32(g_scan + i * 4) & 0x0FFFFFFF;
            if (v == 0)                 st->free_cnt++;
            else if (v == FAT32_BAD)    st->bad_cnt++;
            else {
                st->used_cnt++;
                if (!FK_IS_EOC(v) && v > g->total_clusters + 1) {
                    st->invalid_links++;
                    if (ctx->verbose && shown < 8) {
                        line[0] = 0;
                        l_cat(line, "FAT[", LINE_MAX);
                        l_num(line, c, LINE_MAX);
                        l_cat(line, "] -> out-of-range ", LINE_MAX);
                        l_hex(line, v, LINE_MAX);
                        fsck_note(ctx, line);
                        shown++;
                    }
                    if (fsck_should_fix(ctx)) {
                        if (fat_set(dev, g, c, FAT32_EOC_MIN) == 0)
                            ctx->fixed++;
                        else
                            fsck_err(ctx, "FAT truncate write failed");
                    }
                }
            }
        }
        fsck_yield(ctx);
        if (fsck_break(ctx)) return -1;
    }
    return 0;
}

/* =========================================================================
 * Phase 5 — directory tree walk
 * ========================================================================= */

#define FK_MAX_DEPTH 32

typedef struct {
    uint32_t cluster;
    uint32_t parent;        /* first cluster of the parent directory */
    int      path_off;      /* path length before this frame's "/name" */
    char     name[12];      /* 8.3 name rendered, or "" for the root */
} FkFrame;

static FkFrame g_stack[FK_MAX_DEPTH];
static int     g_sp;
static char    g_path[128];

/* Render an 11-byte short name field as "NAME.EXT". */
static void nm83(const uint8_t *de, char *out)
{
    int k = 0;
    for (int b = 0; b < 8 && de[b] != ' '; b++) out[k++] = (char)de[b];
    if (de[8] != ' ') {
        out[k++] = '.';
        for (int b = 8; b < 11 && de[b] != ' '; b++) out[k++] = (char)de[b];
    }
    out[k] = 0;
}

typedef struct {
    uint32_t files, dirs, deleted, lfn, vol_lbl;
    uint32_t freed;         /* clusters released by repair fixes */
    uint32_t claimed;       /* fresh clusters allocated by repair fixes */
} FkDirStat;

/* Result of walking one cluster chain. */
#define CH_OK        0
#define CH_TRUNC     1   /* chain was truncated (cross-link/loop/bad link) */
#define CH_EMPTY     2   /* start cluster invalid                          */

/* Walk the chain starting at `start`, marking every member in g_ref.
 * On a hit against an already-referenced cluster or an invalid link the
 * chain is truncated (FAT[prev]=EOC) when fixing is enabled.
 * `*out_len` receives the surviving chain length. */
static int walk_chain(BlockDev *dev, FkGeo *g, FsckCtx *ctx,
                      uint32_t start, uint32_t *out_len, const char *what)
{
    char line[LINE_MAX];
    uint32_t cur = start, prev = 0, len = 0;
    uint32_t guard = g->total_clusters + 2;
    int rc = CH_OK;

    while (cur >= 2 && cur <= g->total_clusters + 1 && guard-- > 0) {
        if (ref_get(cur)) {
            line[0] = 0;
            l_cat(line, what, LINE_MAX);
            l_cat(line, ": cluster chain cross-links at ", LINE_MAX);
            l_num(line, cur, LINE_MAX);
            fsck_err(ctx, line);
            rc = CH_TRUNC;
            if (fsck_should_fix(ctx)) {
                if (prev && fat_set(dev, g, prev, FAT32_EOC_MIN) == 0)
                    ctx->fixed++;
            }
            break;
        }
        ref_set(cur);
        len++;
        prev = cur;
        uint32_t next = fat_get(dev, g, cur);
        if (FK_IS_EOC(next)) break;
        if (next == FAT32_BAD) {
            line[0] = 0;
            l_cat(line, what, LINE_MAX);
            l_cat(line, ": chain leads into bad cluster ", LINE_MAX);
            l_num(line, next, LINE_MAX);
            fsck_err(ctx, line);
            rc = CH_TRUNC;
            if (fsck_should_fix(ctx)) {
                if (fat_set(dev, g, cur, FAT32_EOC_MIN) == 0) ctx->fixed++;
            }
            break;
        }
        if (next < 2 || next > g->total_clusters + 1) {
            line[0] = 0;
            l_cat(line, what, LINE_MAX);
            l_cat(line, ": invalid link ", LINE_MAX);
            l_hex(line, next, LINE_MAX);
            l_cat(line, " at cluster ", LINE_MAX);
            l_num(line, cur, LINE_MAX);
            fsck_err(ctx, line);
            rc = CH_TRUNC;
            if (fsck_should_fix(ctx)) {
                if (fat_set(dev, g, cur, FAT32_EOC_MIN) == 0) ctx->fixed++;
            }
            break;
        }
        cur = next;
    }
    if (guard == 0) {
        line[0] = 0;
        l_cat(line, what, LINE_MAX);
        l_cat(line, ": cluster chain unbounded (cut at guard)", LINE_MAX);
        fsck_err(ctx, line);
        if (fsck_should_fix(ctx)) {
            if (prev && fat_set(dev, g, prev, FAT32_EOC_MIN) == 0) ctx->fixed++;
        }
        rc = CH_TRUNC;
    }
    if (out_len) *out_len = len;
    return rc;
}

/* Validate the 11-byte name field of a short dir entry.  Returns 1 when
 * every character is legal; repairs substitute '_'. */
static int name_legal(const uint8_t *de)
{
    if (de[0] == ' ') return 0;             /* leading space illegal */
    for (int i = 0; i < 11; i++) {
        uint8_t c = de[i];
        if (c < 0x20 && c != 0x05) return 0;
        switch (c) {
            case '"': case '*': case '+': case ',': case '/':
            case ':': case ';': case '<': case '=': case '>':
            case '?': case '[': case '\\': case ']': case '|':
                return 0;
        }
    }
    return 1;
}

static int push_frame(uint32_t cluster, uint32_t parent, const char *name)
{
    if (g_sp >= FK_MAX_DEPTH) return -1;
    g_stack[g_sp].cluster  = cluster;
    g_stack[g_sp].parent   = parent;
    g_stack[g_sp].path_off = l_slen(g_path);
    if (name) {
        int i = 0;
        while (i < 11 && name[i]) {
            g_stack[g_sp].name[i] = name[i]; i++;
        }
        g_stack[g_sp].name[i] = 0;
    } else {
        g_stack[g_sp].name[0] = 0;
    }
    g_sp++;
    return 0;
}

static FkFrame pop_frame(void)
{
    g_sp--;
    /* Rebuild g_path: truncate to the length it had when this frame was
     * pushed, then append this frame's own name.  Because pushes never
     * touch g_path, siblings share the same path_off and DFS ordering
     * keeps paths correct. */
    g_path[g_stack[g_sp].path_off] = 0;
    if (g_stack[g_sp].name[0]) {
        l_cat(g_path, "/", sizeof(g_path));
        l_cat(g_path, g_stack[g_sp].name, sizeof(g_path));
    }
    return g_stack[g_sp];
}

/* Find the first free cluster (FAT entry == 0).  Returns 0 when the
 * volume is full.  Reads through g_dirw — callers hold dir data in g_scan. */
static uint32_t alloc_cluster(BlockDev *dev, FkGeo *g, FsckCtx *ctx)
{
    for (uint32_t s = 0; s < g->fat_sz; s += 128) {
        uint32_t n = (g->fat_sz - s > 128) ? 128 : g->fat_sz - s;
        if (fk_rd(dev, g->fat_start + s, g_dirw, n) != 0) {
            fsck_err(ctx, "cannot scan FAT for a free cluster");
            return 0;
        }
        uint32_t ents = n * (g->bps / 4);
        for (uint32_t i = 0; i < ents; i++) {
            uint32_t c = s * (g->bps / 4) + i;
            if (c < 2 || c > g->total_clusters + 1) continue;
            if (fk_le32(g_dirw + i * 4) == 0) {
                if (fat_set(dev, g, c, FAT32_EOC_MIN) != 0) {
                    fsck_err(ctx, "free-cluster claim write failed");
                    return 0;
                }
                return c;   /* caller's dir walk ref-marks it */
            }
        }
    }
    return 0;
}

/* Initialise a fresh cluster as an empty directory ('.' and '..' entries).
 * Uses g_dirw — g_scan may be holding live directory data. */
static int init_dir_cluster(BlockDev *dev, FkGeo *g,
                            uint32_t clus, uint32_t parent)
{
    memset(g_dirw, 0, sizeof(g_dirw));
    memset(g_dirw,      ' ', 11); g_dirw[0]      = '.';
    memset(g_dirw + 32, ' ', 11); g_dirw[32]     = '.';
    g_dirw[33] = '.';
    g_dirw[11] = FAT32_ATTR_DIRECTORY;
    g_dirw[43] = FAT32_ATTR_DIRECTORY;
    fk_put16(g_dirw + 20, (uint16_t)(clus >> 16));
    fk_put16(g_dirw + 26, (uint16_t)(clus & 0xFFFF));
    fk_put16(g_dirw + 52, (uint16_t)(parent >> 16));
    fk_put16(g_dirw + 58, (uint16_t)(parent & 0xFFFF));
    return BlockDev_Write(dev, clus_to_sec(g, clus), g_dirw, g->spc);
}

/* Compose "<current path>/<name>" for issue messages — converts the
 * 8.3 name field to "NAME.EXT" form. */
static void ent_label(char *out, int max, const uint8_t *de)
{
    char nm[16];
    nm83(de, nm);
    out[0] = 0;
    l_cat(out, g_path, max);
    l_cat(out, "/", max);
    l_cat(out, nm, max);
}

/* Scan one directory (the chain rooted at dir_cluster). */
static int scan_dir(BlockDev *dev, FkGeo *g, FsckCtx *ctx,
                    uint32_t dir_cluster, uint32_t parent_cluster,
                    FkDirStat *st)
{
    char line[LINE_MAX], what[LINE_MAX];

    /* Mark the directory's own chain (detects loops/cross-links and keeps
     * the lost-cluster pass from flagging directory space). */
    what[0] = 0;
    l_cat(what, g_path[0] ? g_path : "/", LINE_MAX);
    l_cat(what, " (directory)", LINE_MAX);
    walk_chain(dev, g, ctx, dir_cluster, NULL, what);

    /* Re-walk the (possibly truncated) chain and scan entries. */
    uint32_t cur = dir_cluster, guard = g->total_clusters + 2;
    int is_root = (dir_cluster == g->root_clus);

    while (cur >= 2 && cur <= g->total_clusters + 1 && guard-- > 0) {
        uint32_t sec = clus_to_sec(g, cur);
        if (fk_rd(dev, sec, g_scan, g->spc) != 0) {
            fsck_err(ctx, "cannot read directory cluster");
            return -1;
        }
        uint32_t ents = g->clus_size / FK_DIR_ENT;
        int eod = 0;
        for (uint32_t e = 0; e < ents; e++) {
            uint8_t *de = g_scan + e * FK_DIR_ENT;
            if (de[0] == 0x00) { eod = 1; break; }
            if (de[0] == 0xE5) { st->deleted++; continue; }
            uint8_t attr = de[11];
            if ((attr & 0x0F) == 0x0F) { st->lfn++; continue; }
            if ((attr & 0x18) == 0x08) {
                st->vol_lbl++;
                if (!is_root)
                    fsck_warn(ctx, "volume-label entry outside root directory");
                continue;
            }

            uint32_t start = fk_le16(de + 26) | ((uint32_t)fk_le16(de + 20) << 16);
            uint32_t fsize = fk_le32(de + 28);
            int is_dir = (attr & FAT32_ATTR_DIRECTORY) != 0;

            /* dot entries */
            if (de[0] == '.' && de[1] == ' ' && de[2] == ' ') {
                if (is_root) {
                    fsck_warn(ctx, "'.' entry in root directory");
                } else if (start != dir_cluster) {
                    line[0] = 0;
                    l_cat(line, g_path, LINE_MAX);
                    l_cat(line, ": '.' points at wrong cluster", LINE_MAX);
                    fsck_err(ctx, line);
                    if (fsck_should_fix(ctx)) {
                        if (dirent_set_cluster(dev, g, cur, e,
                                               dir_cluster) == 0)
                            ctx->fixed++;
                    }
                }
                continue;
            }
            if (de[0] == '.' && de[1] == '.' && de[2] == ' ') {
                /* '..' may legally hold cluster 0 when the parent is the
                 * root directory (FAT spec allows either form). */
                int parent_ok =
                    (start == parent_cluster) ||
                    (start == 0 && parent_cluster == g->root_clus);
                if (is_root) {
                    fsck_warn(ctx, "'..' entry in root directory");
                } else if (!parent_ok) {
                    line[0] = 0;
                    l_cat(line, g_path, LINE_MAX);
                    l_cat(line, ": '..' points at wrong cluster", LINE_MAX);
                    fsck_err(ctx, line);
                    if (fsck_should_fix(ctx)) {
                        if (dirent_set_cluster(dev, g, cur, e,
                                               parent_cluster) == 0)
                            ctx->fixed++;
                    }
                }
                continue;
            }

            ent_label(what, LINE_MAX, de);

            /* illegal name characters */
            if (!name_legal(de)) {
                line[0] = 0;
                l_cat(line, what, LINE_MAX);
                l_cat(line, ": illegal characters in name", LINE_MAX);
                fsck_err(ctx, line);
                if (fsck_should_fix(ctx)) {
                    uint8_t fixnm[11];
                    memcpy(fixnm, de, 11);
                    for (int b = 0; b < 11; b++) {
                        uint8_t c = fixnm[b];
                        int bad = (c < 0x20 && c != 0x05);
                        if (!bad) switch (c) {
                            case '"': case '*': case '+': case ',': case '/':
                            case ':': case ';': case '<': case '=': case '>':
                            case '?': case '[': case '\\': case ']': case '|':
                                bad = 1;
                        }
                        if (bad) fixnm[b] = '_';
                    }
                    if (fixnm[0] == ' ') fixnm[0] = '_';
                    if (dirent_patch(dev, g, cur, e, 0, fixnm, 11) == 0)
                        ctx->fixed++;
                }
            }

            /* attribute bits beyond the defined set */
            if (attr & ~0x3F) {
                line[0] = 0;
                l_cat(line, what, LINE_MAX);
                l_cat(line, ": reserved attribute bits set", LINE_MAX);
                fsck_warn(ctx, line);
                if (fsck_should_fix(ctx)) {
                    uint8_t a = attr & 0x3F;
                    if (dirent_patch(dev, g, cur, e, 11, &a, 1) == 0)
                        ctx->fixed++;
                }
            }

            /* start cluster sanity */
            int start_bad = (start == 1 || start > g->total_clusters + 1);
            if (!start_bad && start >= 2 &&
                fat_get(dev, g, start) == 0) {
                /* entry claims a cluster the FAT says is free */
                start_bad = 1;
            }
            if (start_bad) {
                line[0] = 0;
                l_cat(line, what, LINE_MAX);
                l_cat(line, ": invalid start cluster ", LINE_MAX);
                l_num(line, start, LINE_MAX);
                fsck_err(ctx, line);
                if (fsck_should_fix(ctx)) {
                    if (dirent_set_cluster(dev, g, cur, e, 0) == 0 &&
                        dirent_set_size(dev, g, cur, e, 0) == 0)
                        ctx->fixed++;
                }
                continue;
            }

            if (is_dir) {
                st->dirs++;
                if (start == 0) {
                    line[0] = 0;
                    l_cat(line, what, LINE_MAX);
                    l_cat(line, ": directory has no cluster chain", LINE_MAX);
                    fsck_err(ctx, line);
                    if (fsck_should_fix(ctx)) {
                        uint32_t nc = alloc_cluster(dev, g, ctx);
                        if (nc &&
                            init_dir_cluster(dev, g, nc, dir_cluster) == 0 &&
                            dirent_set_cluster(dev, g, cur, e, nc) == 0) {
                            ctx->fixed++;
                            st->claimed++;
                            char nm[16];
                            nm83(de, nm);
                            if (push_frame(nc, dir_cluster, nm) != 0)
                                fsck_warn(ctx, "directory nesting too deep — subtree skipped");
                        } else if (!nc) {
                            fsck_err(ctx, "no free cluster to repair directory");
                        }
                    }
                    continue;
                }
                if (ref_get(start)) {
                    line[0] = 0;
                    l_cat(line, what, LINE_MAX);
                    l_cat(line, ": subdirectory cross-linked — not recursing", LINE_MAX);
                    fsck_err(ctx, line);
                    continue;
                }
                char nm[16];
                nm83(de, nm);
                if (push_frame(start, dir_cluster, nm) != 0) {
                    fsck_warn(ctx, "directory nesting too deep — subtree skipped");
                }
            } else {
                st->files++;
                if (fsize == 0 && start != 0) {
                    line[0] = 0;
                    l_cat(line, what, LINE_MAX);
                    l_cat(line, ": empty file owns cluster chain at ", LINE_MAX);
                    l_num(line, start, LINE_MAX);
                    fsck_err(ctx, line);
                    /* Always mark the orphaned chain as seen so the
                     * lost-cluster pass doesn't report it a second time. */
                    {
                        uint32_t fc = start, fg = g->total_clusters + 2;
                        while (fc >= 2 && fc <= g->total_clusters + 1 && fg--) {
                            ref_set(fc);
                            uint32_t nx = fat_get(dev, g, fc);
                            if (FK_IS_EOC(nx)) break;
                            if (nx < 2 || nx > g->total_clusters + 1) break;
                            fc = nx;
                        }
                    }
                    if (fsck_should_fix(ctx)) {
                        uint32_t fc = start, fg = g->total_clusters + 2;
                        while (fc >= 2 && fc <= g->total_clusters + 1 && fg--) {
                            uint32_t nx = fat_get(dev, g, fc);
                            if (fat_set(dev, g, fc, 0) == 0)
                                st->freed++;
                            if (FK_IS_EOC(nx)) break;
                            fc = nx;
                        }
                        if (dirent_set_cluster(dev, g, cur, e, 0) == 0)
                            ctx->fixed++;
                    }
                    continue;
                }
                if (fsize != 0 && start == 0) {
                    line[0] = 0;
                    l_cat(line, what, LINE_MAX);
                    l_cat(line, ": nonzero size with no cluster chain", LINE_MAX);
                    fsck_err(ctx, line);
                    if (fsck_should_fix(ctx)) {
                        if (dirent_set_size(dev, g, cur, e, 0) == 0)
                            ctx->fixed++;
                    }
                    continue;
                }
                if (start >= 2) {
                    uint32_t chain_len = 0;
                    int xlinked = ref_get(start);
                    if (xlinked) {
                        line[0] = 0;
                        l_cat(line, what, LINE_MAX);
                        l_cat(line, ": first cluster already owned (cross-link)", LINE_MAX);
                        fsck_err(ctx, line);
                        if (fsck_should_fix(ctx)) {
                            if (dirent_set_cluster(dev, g, cur, e, 0) == 0 &&
                                dirent_set_size(dev, g, cur, e, 0) == 0)
                                ctx->fixed++;
                        }
                        continue;
                    }
                    walk_chain(dev, g, ctx, start, &chain_len, what);
                    uint32_t need = (fsize + g->clus_size - 1) / g->clus_size;
                    if (chain_len < need) {
                        line[0] = 0;
                        l_cat(line, what, LINE_MAX);
                        l_cat(line, ": size ", LINE_MAX);
                        l_num(line, fsize, LINE_MAX);
                        l_cat(line, " exceeds chain (", LINE_MAX);
                        l_num(line, chain_len, LINE_MAX);
                        l_cat(line, " clusters)", LINE_MAX);
                        fsck_err(ctx, line);
                        if (fsck_should_fix(ctx)) {
                            if (dirent_set_size(dev, g, cur, e,
                                                chain_len * g->clus_size) == 0)
                                ctx->fixed++;
                        }
                    } else if (chain_len > need && need > 0) {
                        line[0] = 0;
                        l_cat(line, what, LINE_MAX);
                        l_cat(line, ": chain has ", LINE_MAX);
                        l_num(line, chain_len - need, LINE_MAX);
                        l_cat(line, " excess clusters", LINE_MAX);
                        fsck_warn(ctx, line);
                        if (fsck_should_fix(ctx)) {
                            /* free the tail beyond `need` clusters */
                            uint32_t c = start;
                            for (uint32_t i = 1; i < need && c >= 2; i++)
                                c = fat_get(dev, g, c);
                            uint32_t tail = (c >= 2) ? fat_get(dev, g, c) : 0;
                            if (c >= 2 && tail >= 2 &&
                                tail <= g->total_clusters + 1) {
                                fat_set(dev, g, c, FAT32_EOC_MIN);
                                uint32_t fg = g->total_clusters + 2;
                                while (tail >= 2 &&
                                       tail <= g->total_clusters + 1 &&
                                       fg--) {
                                    uint32_t nx = fat_get(dev, g, tail);
                                    ref_set(tail);
                                    if (fat_set(dev, g, tail, 0) == 0)
                                        st->freed++;
                                    tail = nx;
                                }
                                ctx->fixed++;
                            }
                        }
                    }
                }
            }
        }
        if (eod) break;
        uint32_t nx = fat_get(dev, g, cur);
        if (FK_IS_EOC(nx) || nx < 2 || nx > g->total_clusters + 1) break;
        cur = nx;
    }
    return 0;
}

/* =========================================================================
 * Phase 6 — lost clusters
 * ========================================================================= */
static int phase_lost(BlockDev *dev, FkGeo *g, FkFatStat *st,
                      uint32_t *reclaimed, FsckCtx *ctx)
{
    char line[LINE_MAX];
    uint32_t lost = 0;
    (void)st;

    memset(g_lost, 0, (g->total_clusters + 2 + 7) / 8);

    /* Pass 1: mark every cluster that is used in the FAT but was not
     * reached by the directory walk. */
    for (uint32_t s = 0; s < g->fat_sz; s += 128) {
        uint32_t n = (g->fat_sz - s > 128) ? 128 : g->fat_sz - s;
        if (fk_rd(dev, g->fat_start + s, g_scan, n) != 0) {
            fsck_err(ctx, "cannot re-read FAT for lost-chain pass");
            return -1;
        }
        uint32_t ents = n * (g->bps / 4);
        for (uint32_t i = 0; i < ents; i++) {
            uint32_t c = s * (g->bps / 4) + i;
            if (c < 2 || c > g->total_clusters + 1) continue;
            uint32_t v = fk_le32(g_scan + i * 4) & 0x0FFFFFFF;
            if (v == 0 || v == FAT32_BAD) continue;
            if (ref_get(c)) continue;
            lost_set(c);
            lost++;
        }
        fsck_yield(ctx);
        if (fsck_break(ctx)) return -1;
    }
    if (!lost) return 0;

    line[0] = 0;
    l_cat(line, "lost clusters: ", LINE_MAX);
    l_num(line, lost, LINE_MAX);
    l_cat(line, " (", LINE_MAX);
    {
        char sz[24];
        fk_human((uint64_t)lost * g->clus_size, sz, sizeof(sz));
        l_cat(line, sz, LINE_MAX);
    }
    l_cat(line, ") unreachable from any directory", LINE_MAX);
    fsck_err(ctx, line);

    if (fsck_should_fix(ctx)) {
        /* Pass 2: free every cluster marked in pass 1. */
        uint32_t freed = 0;
        for (uint32_t s = 0; s < g->fat_sz; s += 128) {
            uint32_t n = (g->fat_sz - s > 128) ? 128 : g->fat_sz - s;
            if (fk_rd(dev, g->fat_start + s, g_scan, n) != 0)
                return -1;
            uint32_t ents = n * (g->bps / 4);
            for (uint32_t i = 0; i < ents; i++) {
                uint32_t c = s * (g->bps / 4) + i;
                if (c < 2 || c > g->total_clusters + 1) continue;
                if (lost_get(c) && fat_set(dev, g, c, 0) == 0)
                    freed++;
            }
            fsck_yield(ctx);
            if (fsck_break(ctx)) return -1;
        }
        *reclaimed = freed;
        ctx->fixed++;
        line[0] = 0;
        l_cat(line, "  ", LINE_MAX);
        l_num(line, freed, LINE_MAX);
        l_cat(line, " lost clusters marked free", LINE_MAX);
        fsck_out(ctx, line);
    }
    return 0;
}

/* =========================================================================
 * Public entry points
 * ========================================================================= */
int FSCK_FAT32_Probe(BlockDev *dev)
{
    static uint8_t probe_sec[512] __attribute__((aligned(4096)));
    if (BlockDev_Read(dev, 0, probe_sec, 1) != 0) return 0;
    if (memcmp(probe_sec + 3, "EXFAT   ", 8) == 0) return 0;
    return bpb_valid(probe_sec);
}

int FSCK_FAT32_Info(BlockDev *dev, FsckCtx *ctx)
{
    char line[LINE_MAX];
    if (BlockDev_Read(dev, 0, g_sec, 1) != 0) {
        fsck_err(ctx, "cannot read boot sector");
        return -1;
    }
    if (!bpb_valid(g_sec)) {
        fsck_err(ctx, "not a valid FAT32 BPB");
        return -1;
    }
    FkGeo g;
    geo_from_bpb(&g, g_sec, dev->num_sectors);

    char oem[9]; memcpy(oem, g_sec + 3, 8); oem[8] = 0;
    char lbl[12]; memcpy(lbl, g_sec + 71, 11); lbl[11] = 0;
    int ll = 10; while (ll >= 0 && lbl[ll] == ' ') lbl[ll--] = 0;

    line[0] = 0;
    l_cat(line, "  OEM '", LINE_MAX); l_cat(line, oem, LINE_MAX);
    l_cat(line, "'  label '", LINE_MAX); l_cat(line, lbl[0] ? lbl : "<none>", LINE_MAX);
    l_cat(line, "'  serial ", LINE_MAX); l_hex(line, fk_le32(g_sec + 67), LINE_MAX);
    fsck_out(ctx, line);

    line[0] = 0;
    l_cat(line, "  bytes/sec ", LINE_MAX); l_num(line, g.bps, LINE_MAX);
    l_cat(line, "  sec/clus ", LINE_MAX); l_num(line, g.spc, LINE_MAX);
    l_cat(line, " (", LINE_MAX);
    {
        char sz[24]; fk_human(g.clus_size, sz, sizeof(sz)); l_cat(line, sz, LINE_MAX);
    }
    l_cat(line, "/cluster)  fats ", LINE_MAX); l_num(line, g.nfats, LINE_MAX);
    l_cat(line, " x ", LINE_MAX); l_num(line, g.fat_sz, LINE_MAX);
    l_cat(line, " secs @", LINE_MAX); l_num(line, g.fat_start, LINE_MAX);
    fsck_out(ctx, line);

    line[0] = 0;
    l_cat(line, "  reserved ", LINE_MAX); l_num(line, g.rsvd, LINE_MAX);
    l_cat(line, "  data start ", LINE_MAX); l_num(line, g.data_start, LINE_MAX);
    l_cat(line, "  tot secs ", LINE_MAX); l_num(line, g.tot_sec, LINE_MAX);
    l_cat(line, "  root clus ", LINE_MAX); l_num(line, g.root_clus, LINE_MAX);
    fsck_out(ctx, line);

    line[0] = 0;
    l_cat(line, "  clusters ", LINE_MAX); l_num(line, g.total_clusters, LINE_MAX);
    l_cat(line, "  volume size ", LINE_MAX);
    {
        char sz[24];
        fk_human((uint64_t)g.total_clusters * g.clus_size, sz, sizeof(sz));
        l_cat(line, sz, LINE_MAX);
    }
    l_cat(line, "  fsinfo @", LINE_MAX); l_num(line, g.fsinfo_sec, LINE_MAX);
    l_cat(line, "  backup @", LINE_MAX); l_num(line, g.bk_boot, LINE_MAX);
    fsck_out(ctx, line);

    /* FSINFO block */
    if (g.fsinfo_sec < g.rsvd &&
        BlockDev_Read(dev, g.fsinfo_sec, g_sec2, 1) == 0 &&
        fk_le32(g_sec2) == 0x41615252) {
        line[0] = 0;
        l_cat(line, "  FSINFO free ", LINE_MAX);
        uint32_t fc = fk_le32(g_sec2 + 488);
        if (fc == 0xFFFFFFFF) l_cat(line, "unknown", LINE_MAX);
        else l_num(line, fc, LINE_MAX);
        l_cat(line, "  next_free ", LINE_MAX);
        uint32_t nf = fk_le32(g_sec2 + 492);
        if (nf == 0xFFFFFFFF) l_cat(line, "unknown", LINE_MAX);
        else l_num(line, nf, LINE_MAX);
        fsck_out(ctx, line);
    }
    return 0;
}

int FSCK_FAT32_Check(BlockDev *dev, FsckCtx *ctx)
{
    FkGeo g;
    FkFsInfo fi;
    FkFatStat st;
    FkDirStat dst;
    uint32_t reclaimed = 0;
    char line[LINE_MAX];

    memset(&dst, 0, sizeof(dst));
    g_fatc_sec = 0xFFFFFFFF;   /* drop FAT-sector cache from any prior run */

    /* --- 1. BPB --- */
    fsck_note(ctx, "phase 1: boot sector");
    if (phase_bpb(dev, &g, ctx) != 0) {
        fsck_err(ctx, "fatal: cannot establish FAT32 geometry");
        return -1;
    }

    /* --- 2. FSINFO --- */
    fsck_note(ctx, "phase 2: FSINFO");
    phase_fsinfo(dev, &g, &fi, ctx);

    /* --- 3. FAT copies --- */
    fsck_note(ctx, "phase 3: FAT copies");
    if (phase_fat_compare(dev, &g, ctx) < 0) {
        fsck_out(ctx, "check aborted");
        return -1;
    }

    /* --- 4. FAT scan --- */
    fsck_note(ctx, "phase 4: FAT scan");
    if (phase_fat_scan(dev, &g, &st, ctx) != 0) {
        fsck_out(ctx, "check aborted");
        return -1;
    }

    /* --- 5. directory tree --- */
    fsck_note(ctx, "phase 5: directory tree");
    g_sp = 0;
    g_path[0] = 0;
    push_frame(g.root_clus, 0, NULL);   /* root — path stays "" */
    while (g_sp > 0) {
        FkFrame fr = pop_frame();
        scan_dir(dev, &g, ctx, fr.cluster, fr.parent, &dst);
        if (ctx->aborted) { fsck_out(ctx, "check aborted"); return -1; }
        fsck_yield(ctx);
    }

    /* --- 6. lost clusters --- */
    fsck_note(ctx, "phase 6: lost clusters");
    if (phase_lost(dev, &g, &st, &reclaimed, ctx) != 0) {
        fsck_out(ctx, "check aborted");
        return -1;
    }

    /* --- 7. reconcile free space + FSINFO --- */
    uint32_t real_free = st.free_cnt + reclaimed + dst.freed - dst.claimed;
    if (fi.present) {
        int need_fix = 0;
        if (!fi.sig_ok) need_fix = 1;
        else if (fi.free_cnt != 0xFFFFFFFF && fi.free_cnt != real_free) {
            line[0] = 0;
            l_cat(line, "FSINFO free count ", LINE_MAX);
            l_num(line, fi.free_cnt, LINE_MAX);
            l_cat(line, " != actual ", LINE_MAX);
            l_num(line, real_free, LINE_MAX);
            fsck_err(ctx, line);
            need_fix = 1;
        }
        if (need_fix && fsck_should_fix(ctx)) {
            if (fsinfo_fix(dev, &g, real_free, ctx) == 0) {
                ctx->fixed++;
                fsck_out(ctx, "  FSINFO rewritten");
            } else {
                fsck_err(ctx, "FSINFO write failed");
            }
        }
    } else if (ctx->verbose) {
        fsck_note(ctx, "no FSINFO sector");
    }

    /* --- summary --- */
    fsck_out(ctx, "");
    line[0] = 0;
    l_cat(line, "Files ", LINE_MAX);  l_num(line, dst.files, LINE_MAX);
    l_cat(line, "  dirs ", LINE_MAX); l_num(line, dst.dirs, LINE_MAX);
    l_cat(line, "  del ", LINE_MAX);  l_num(line, dst.deleted, LINE_MAX);
    l_cat(line, "  lfn ", LINE_MAX);  l_num(line, dst.lfn, LINE_MAX);
    fsck_out(ctx, line);
    line[0] = 0;
    l_cat(line, "Clusters: ", LINE_MAX);
    l_num(line, g.total_clusters, LINE_MAX);
    l_cat(line, " total, ", LINE_MAX);
    l_num(line, real_free, LINE_MAX);
    l_cat(line, " free, ", LINE_MAX);
    l_num(line, st.used_cnt, LINE_MAX);
    l_cat(line, " used, ", LINE_MAX);
    l_num(line, st.bad_cnt, LINE_MAX);
    l_cat(line, " bad", LINE_MAX);
    if (st.invalid_links) {
        l_cat(line, ", ", LINE_MAX);
        l_num(line, st.invalid_links, LINE_MAX);
        l_cat(line, " bad links", LINE_MAX);
    }
    fsck_out(ctx, line);
    return 0;
}
