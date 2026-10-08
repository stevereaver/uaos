/*
 * fat32.c — UAOS FAT32 Filesystem Driver Implementation
 *
 * Implements read/write FAT32 filesystem support for block devices.
 * Supports: file open/create/read/write, directory create/delete/list,
 * cluster allocation/deallocation, and path traversal.
 */

#include "fat32.h"
#include "amiga_dos_types.h"
#include "../irq/rtc.h"
#include <stdio.h>
#include <string.h>

/* =========================================================================
 * Static allocation (no malloc in freestanding)
 * ========================================================================= */

static Fat32FS g_fat32_fs;

/* Pool of file handles — the handler layer indexes into this */
#define FAT32_MAX_FILES 16
static Fat32File g_fat32_files[FAT32_MAX_FILES];

/* Sector buffer for FAT and directory operations — 4K-aligned for DMA */
static uint8_t g_sector_buf[512]   __attribute__((aligned(4096)));
static uint8_t g_sector_buf2[512]  __attribute__((aligned(4096)));

/* Cluster buffer (max reasonable cluster size = 64 KB for 128 sec/clus) */
#define FAT32_MAX_CLUSTER_SECS 128
static uint8_t g_cluster_buf[FAT32_MAX_CLUSTER_SECS * 512] __attribute__((aligned(4096)));

/* Directory-iteration buffer, private to FAT32_ReadDir so an ExNext loop
 * survives other I/O between calls.  g_dir_buf_cluster tags the cached
 * cluster; every write through fat32_bwrite() drops the cache. */
static uint8_t g_dir_buf[FAT32_MAX_CLUSTER_SECS * 512] __attribute__((aligned(4096)));
static uint32_t g_dir_buf_cluster;

static int fat32_bwrite(BlockDev *bdev, uint64_t sector, const void *buf,
                        uint32_t count)
{
    g_dir_buf_cluster = 0;
    return BlockDev_Write(bdev, sector, buf, count);
}

/* =========================================================================
 * Little-endian helpers
 * ========================================================================= */

static inline uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline uint32_t le32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void put_le16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
}
static inline void put_le32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

/* FAT32 EOC (end-of-chain) markers */
#define FAT32_EOC      0x0FFFFFFF
#define FAT32_EOC_MIN  0x0FFFFFF8
#define FAT32_IS_EOC(c)  ((c) >= FAT32_EOC_MIN)
#define FAT32_IS_FREE(c) ((c) == 0)

/* =========================================================================
 * File handle pool
 * ========================================================================= */

static Fat32File *fat32_alloc_file(void)
{
    for (int i = 0; i < FAT32_MAX_FILES; i++) {
        if (!g_fat32_files[i].in_use) {
            memset(&g_fat32_files[i], 0, sizeof(Fat32File));
            g_fat32_files[i].in_use = 1;
            return &g_fat32_files[i];
        }
    }
    printf("[FAT32] File handle pool exhausted\n");
    return NULL;
}

/* =========================================================================
 * Cluster / sector conversion
 * ========================================================================= */

static uint32_t fat32_cluster_to_sector(Fat32FS *fs, uint32_t cluster)
{
    return fs->data_start + (cluster - 2) * fs->sec_per_clus;
}

static int fat32_read_cluster(Fat32FS *fs, uint32_t cluster, uint8_t *buf)
{
    if (cluster < 2) return -1;
    uint32_t sec = fat32_cluster_to_sector(fs, cluster);
    for (uint32_t i = 0; i < fs->sec_per_clus; i++) {
        if (BlockDev_Read(fs->bdev, sec + i,
                          buf + i * fs->bytes_per_sec, 1) != 0)
            return -1;
    }
    return 0;
}

static int fat32_write_cluster(Fat32FS *fs, uint32_t cluster, const uint8_t *buf)
{
    if (cluster < 2) return -1;
    uint32_t sec = fat32_cluster_to_sector(fs, cluster);
    for (uint32_t i = 0; i < fs->sec_per_clus; i++) {
        if (fat32_bwrite(fs->bdev, sec + i,
                           buf + i * fs->bytes_per_sec, 1) != 0)
            return -1;
    }
    return 0;
}

static int fat32_zero_cluster(Fat32FS *fs, uint32_t cluster)
{
    if (cluster < 2) return -1;
    uint32_t sec = fat32_cluster_to_sector(fs, cluster);
    uint32_t remain = fs->sec_per_clus;
    uint32_t s = 0;
    /* Use cluster buffer (zeroed) for batch writes */
    memset(g_cluster_buf, 0, fs->cluster_size);
    while (remain > 0) {
        uint32_t batch = (remain > FAT32_MAX_CLUSTER_SECS) ?
                         FAT32_MAX_CLUSTER_SECS : remain;
        if (fat32_bwrite(fs->bdev, sec + s, g_cluster_buf, batch) != 0)
            return -1;
        s += batch;
        remain -= batch;
    }
    return 0;
}

/* =========================================================================
 * FAT entry read / write
 * ========================================================================= */

static uint32_t fat32_get_fat_entry(Fat32FS *fs, uint32_t cluster)
{
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sec = fs->fat_start + (fat_offset / fs->bytes_per_sec);
    uint32_t fat_off = fat_offset % fs->bytes_per_sec;

    /* Handle 4-byte entry spanning two sectors */
    if (fat_off + 3 >= fs->bytes_per_sec) {
        if (BlockDev_Read(fs->bdev, fat_sec, g_sector_buf, 1) != 0) {
            printf("[FAT32] Failed to read FAT sector %u\n", fat_sec);
            return FAT32_EOC;
        }
        if (BlockDev_Read(fs->bdev, fat_sec + 1, g_sector_buf2, 1) != 0) {
            printf("[FAT32] Failed to read FAT sector %u\n", fat_sec + 1);
            return FAT32_EOC;
        }
        uint32_t entry = g_sector_buf[fat_off] |
                         (g_sector_buf[fat_off + 1] << 8);
        uint32_t remain = fs->bytes_per_sec - fat_off;
        entry |= (uint32_t)g_sector_buf2[0] << (remain * 8);
        if (remain == 2)
            entry |= (uint32_t)g_sector_buf2[1] << 16;
        return entry & 0x0FFFFFFF;
    }

    if (BlockDev_Read(fs->bdev, fat_sec, g_sector_buf, 1) != 0) {
        printf("[FAT32] Failed to read FAT sector %u\n", fat_sec);
        return FAT32_EOC;
    }
    return le32(&g_sector_buf[fat_off]) & 0x0FFFFFFF;
}

/* Write a FAT entry, updating both FAT copies */
static int fat32_set_fat_entry(Fat32FS *fs, uint32_t cluster, uint32_t value)
{
    value &= 0x0FFFFFFF;
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sec = fs->fat_start + (fat_offset / fs->bytes_per_sec);
    uint32_t fat_off = fat_offset % fs->bytes_per_sec;
    uint32_t fat_sz = fs->bpb.fat_sz32;
    uint8_t entry_bytes[4];
    put_le32(entry_bytes, value);

    /* Handle entry spanning two sectors */
    if (fat_off + 3 >= fs->bytes_per_sec) {
        /* Read first sector, modify, write */
        if (BlockDev_Read(fs->bdev, fat_sec, g_sector_buf, 1) != 0) return -1;
        if (BlockDev_Read(fs->bdev, fat_sec + 1, g_sector_buf2, 1) != 0) return -1;
        uint32_t remain = fs->bytes_per_sec - fat_off;
        for (uint32_t i = 0; i < remain; i++)
            g_sector_buf[fat_off + i] = entry_bytes[i];
        for (uint32_t i = 0; i < 4 - remain; i++)
            g_sector_buf2[i] = entry_bytes[remain + i];
        /* Write both FAT copies */
        for (uint32_t f = 0; f < fs->bpb.num_fats; f++) {
            uint32_t base = fs->fat_start + f * fat_sz;
            if (fat32_bwrite(fs->bdev, base + fat_sec - fs->fat_start,
                               g_sector_buf, 1) != 0) return -1;
            if (fat32_bwrite(fs->bdev, base + fat_sec + 1 - fs->fat_start,
                               g_sector_buf2, 1) != 0) return -1;
        }
        return 0;
    }

    /* Normal case: entry within one sector */
    if (BlockDev_Read(fs->bdev, fat_sec, g_sector_buf, 1) != 0) return -1;
    for (int i = 0; i < 4; i++)
        g_sector_buf[fat_off + i] = entry_bytes[i];

    /* Write to all FAT copies */
    for (uint32_t f = 0; f < fs->bpb.num_fats; f++) {
        uint32_t base = fs->fat_start + f * fat_sz;
        if (fat32_bwrite(fs->bdev, base + (fat_sec - fs->fat_start),
                           g_sector_buf, 1) != 0) return -1;
    }
    return 0;
}

/* =========================================================================
 * Cluster allocation / freeing
 * ========================================================================= */

/* The FSINFO free-cluster hint goes stale the moment the FAT changes.
 * Rather than maintain it, mark it unknown (0xFFFFFFFF) once per mount —
 * the spec-sanctioned value; fsck.fat/chkdsk then recount instead of
 * flagging a wrong summary. */
static void fat32_fsinfo_invalidate(Fat32FS *fs)
{
    uint32_t sec = fs->bpb.fs_info;
    if (fs->fsinfo_stale) return;
    fs->fsinfo_stale = 1;
    if (sec == 0 || sec == 0xFFFF || sec >= fs->fat_start) return;
    if (BlockDev_Read(fs->bdev, sec, g_sector_buf, 1) != 0) return;
    if (le32(&g_sector_buf[0]) != 0x41615252 || le32(&g_sector_buf[484]) != 0x61417272)
        return;
    put_le32(&g_sector_buf[488], 0xFFFFFFFF);
    put_le32(&g_sector_buf[492], 0xFFFFFFFF);
    fat32_bwrite(fs->bdev, sec, g_sector_buf, 1);
}

/* Find a free cluster, mark it as EOC, and return its number.
 * Returns 0 on failure (no free clusters). */
static uint32_t fat32_alloc_cluster(Fat32FS *fs)
{
    /* Scan the FAT for a free entry (value == 0) */
    uint32_t fat_sz = fs->bpb.fat_sz32;
    uint32_t total_fat_sectors = fat_sz * fs->bpb.num_fats;

    for (uint32_t sec = 0; sec < fat_sz; sec++) {
        if (BlockDev_Read(fs->bdev, fs->fat_start + sec,
                          g_sector_buf, 1) != 0) break;
        uint32_t ents_per_sec = fs->bytes_per_sec / 4;
        for (uint32_t e = 0; e < ents_per_sec; e++) {
            uint32_t cluster = sec * ents_per_sec + e;
            if (cluster < 2 || cluster >= fs->total_clusters + 2)
                continue;
            uint32_t val = le32(&g_sector_buf[e * 4]) & 0x0FFFFFFF;
            if (val == 0) {
                /* Found a free cluster — mark it as EOC */
                fat32_fsinfo_invalidate(fs);
                if (fat32_set_fat_entry(fs, cluster, FAT32_EOC) != 0)
                    return 0;
                /* Zero the cluster data */
                if (fat32_zero_cluster(fs, cluster) != 0)
                    return 0;
                if (fs->free_clusters > 0)
                    fs->free_clusters--;
                return cluster;
            }
        }
        (void)total_fat_sectors;
    }
    printf("[FAT32] Disk full — no free clusters\n");
    return 0;
}

/* Free an entire cluster chain starting at start_cluster.
 * Bounded by total_clusters: a corrupt FAT could otherwise send us
 * around a cycle forever and hang the calling task. */
static void fat32_free_chain(Fat32FS *fs, uint32_t start_cluster)
{
    uint32_t cluster = start_cluster;
    uint32_t guard = fs->total_clusters + 2;
    if (cluster >= 2) fat32_fsinfo_invalidate(fs);
    while (cluster >= 2 && !FAT32_IS_EOC(cluster) && guard-- > 0) {
        uint32_t next = fat32_get_fat_entry(fs, cluster);
        fat32_set_fat_entry(fs, cluster, 0);
        if (fs->free_clusters >= 0 &&
            fs->free_clusters < (int32_t)fs->total_clusters)
            fs->free_clusters++;
        cluster = next;
    }
}

/* Extend a cluster chain by one cluster. Returns the new cluster, or 0. */
static uint32_t fat32_extend_chain(Fat32FS *fs, uint32_t last_cluster)
{
    uint32_t newc = fat32_alloc_cluster(fs);
    if (newc == 0) return 0;
    /* Link the old last cluster to the new one */
    if (last_cluster >= 2) {
        fat32_set_fat_entry(fs, last_cluster, newc);
    }
    fat32_set_fat_entry(fs, newc, FAT32_EOC);
    return newc;
}

/* =========================================================================
 * Names: 8.3 short names + VFAT long file names (UAOS-254)
 *
 * A long name is stored as a chain of 32-byte LFN entries (attr 0x0F)
 * immediately preceding its 8.3 "alias" entry, last fragment first
 * (ord | 0x40) down to ord 1.  Each fragment carries 13 UCS-2 chars and
 * the checksum of the alias's 11 name bytes, which binds chain to alias.
 * UAOS names are Latin-1, so UCS-2 maps 1:1 for U+0000..U+00FF and any
 * other code point reads back as '_'.
 * ========================================================================= */

/* Directory entry size is always 32 bytes */
#define FAT32_DIR_ENTRY_SIZE 32

#define FAT32_LFN_MAX        255     /* VFAT name length limit (chars) */
#define FAT32_LFN_ENTS_MAX   20      /* ceil(255 / 13) */
#define FAT32_LFN_LAST       0x40    /* ord flag on the final fragment */
#define FAT32_NT_LOWER_BASE  0x08    /* nt_res: 8.3 base displays lowercase */
#define FAT32_NT_LOWER_EXT   0x10    /* nt_res: 8.3 ext displays lowercase */

/* Byte offsets of the 13 UCS-2 chars within an LFN entry */
static const uint8_t g_lfn_offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

/* The freestanding kernel libc has no strchr/strrchr. */
static const char *fat32_strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) return s;
        if (!*s) return NULL;
    }
}

static const char *fat32_strrchr(const char *s, int c)
{
    const char *r = NULL;
    for (; *s; s++)
        if (*s == (char)c) r = s;
    return r;
}

static inline uint8_t l1_upper(uint8_t c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7))
        return (uint8_t)(c - 32);
    return c;
}

static inline uint8_t l1_lower(uint8_t c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
        return (uint8_t)(c + 32);
    return c;
}

/* Case-insensitive Latin-1 compare (AmigaDOS name semantics). */
static int fat32_name_eq(const char *a, const char *b)
{
    while (*a && *b)
        if (l1_upper((uint8_t)*a++) != l1_upper((uint8_t)*b++)) return 0;
    return *a == *b;
}

/* Render a raw entry's 8.3 name, honouring the NT lowercase flags that
 * Windows/Linux set for names like "readme.txt" stored without an LFN. */
static void fat32_83_to_name(const uint8_t *de, char *out)
{
    uint8_t nt = de[12];
    int i, j = 0;
    for (i = 0; i < 8 && de[i] != ' '; i++) {
        uint8_t c = (i == 0 && de[0] == 0x05) ? 0xE5 : de[i];
        out[j++] = (char)((nt & FAT32_NT_LOWER_BASE) ? l1_lower(c) : c);
    }
    if (de[8] != ' ') {
        out[j++] = '.';
        for (i = 8; i < 11 && de[i] != ' '; i++)
            out[j++] = (char)((nt & FAT32_NT_LOWER_EXT) ? l1_lower(de[i]) : de[i]);
    }
    out[j] = '\0';
}

static uint8_t fat32_lfn_checksum(const uint8_t *sfn)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sfn[i]);
    return sum;
}

typedef struct { uint32_t sec, off; } Fat32Slot;

/* LFN chain accumulator, fed every entry of a directory scan in order. */
typedef struct {
    uint16_t  ucs[FAT32_LFN_ENTS_MAX * 13 + 1];
    char      name[FAT32_LFN_MAX + 1];
    uint8_t   chk;          /* checksum carried by the chain */
    uint8_t   total;        /* fragment count (ord of the 0x40 entry) */
    uint8_t   expect;       /* ord of the next fragment; 0 = chain complete */
    uint8_t   active;
    uint8_t   nslots;       /* slots of the chain bound to the last alias */
    Fat32Slot slots[FAT32_LFN_ENTS_MAX];
} Fat32Lfn;

/* Non-reentrant, like the sector/cluster buffers this driver shares. */
static Fat32Lfn g_lfn_find;     /* last fat32_find_in_dir() match */
static Fat32Lfn g_lfn_iter;     /* FAT32_ReadDir() */

static void lfn_reset(Fat32Lfn *l) { l->active = 0; l->nslots = 0; }

static void lfn_feed(Fat32Lfn *l, const uint8_t *de, uint32_t sec, uint32_t off)
{
    uint8_t seq = de[0] & 0x1F;
    if (de[0] & FAT32_LFN_LAST) {
        if (seq == 0 || seq > FAT32_LFN_ENTS_MAX) { lfn_reset(l); return; }
        l->active = 1;
        l->chk = de[13];
        l->total = seq;
        l->nslots = 0;
        l->ucs[seq * 13] = 0;
    } else if (!l->active || seq == 0 || seq != l->expect || de[13] != l->chk) {
        lfn_reset(l);
        return;
    }
    for (int k = 0; k < 13; k++)
        l->ucs[(seq - 1) * 13 + k] = le16(&de[g_lfn_offs[k]]);
    l->slots[l->nslots].sec = sec;
    l->slots[l->nslots].off = off;
    l->nslots++;
    l->expect = (uint8_t)(seq - 1);
}

/* At an 8.3 entry: returns 1 (name in l->name, chain slots in l->slots)
 * when a complete chain whose checksum matches this alias precedes it.
 * Orphaned or mismatched chains are ignored, as VFAT requires. */
static int lfn_finish(Fat32Lfn *l, const uint8_t *de)
{
    int ok = l->active && l->expect == 0 && l->chk == fat32_lfn_checksum(de);
    l->active = 0;
    if (ok) {
        int n = 0, lim = l->total * 13;
        while (n < lim && l->ucs[n] != 0x0000 && l->ucs[n] != 0xFFFF) {
            if (n >= FAT32_LFN_MAX) { ok = 0; break; }
            l->name[n] = (char)(l->ucs[n] < 0x100 ? l->ucs[n] : '_');
            n++;
        }
        l->name[n] = '\0';
        if (n == 0) ok = 0;
    }
    if (!ok) l->nslots = 0;
    return ok;
}

/* Characters legal in an 8.3 name besides A-Z / 0-9. */
static int sfn_char_ok(uint8_t c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        return 1;
    return c && fat32_strchr("$%'-_@~`!(){}^#&", c) != NULL;
}

/* Build the 11-byte 8.3 name for `name`.
 *   0  name is a plain 8.3 name: sfn is exact, *nt carries lowercase flags
 *   1  name needs an LFN chain: sfn is the basis name (tail not yet applied)
 *  -1  name is not representable on FAT */
static int fat32_make_sfn(const char *name, uint8_t *sfn, uint8_t *nt)
{
    int len = (int)strlen(name);
    if (len == 0 || len > FAT32_LFN_MAX) return -1;
    int only_dots_spaces = 1;
    for (int i = 0; i < len; i++) {
        uint8_t c = (uint8_t)name[i];
        if (c < 0x20 || fat32_strchr("\"*/:<>?\\|", c)) return -1;
        if (c != '.' && c != ' ') only_dots_spaces = 0;
    }
    if (only_dots_spaces) return -1;

    memset(sfn, ' ', 11);
    *nt = 0;

    /* Exact 8.3 fit: one optional dot, 1-8 base, 1-3 ext, legal chars,
     * and each half uniformly cased (mixed case needs an LFN). */
    const char *dot = fat32_strchr(name, '.');
    int blen = dot ? (int)(dot - name) : len;
    int elen = dot ? len - blen - 1 : 0;
    int fits = blen >= 1 && blen <= 8 && elen <= 3 && !(dot && elen == 0) &&
               !(dot && fat32_strchr(dot + 1, '.'));
    int up[2] = {0, 0}, lo[2] = {0, 0};
    for (int i = 0; fits && i < len; i++) {
        uint8_t c = (uint8_t)name[i];
        int half = (dot && name + i > dot);
        if (name + i == dot) continue;
        if (!sfn_char_ok(c)) fits = 0;
        else if (c >= 'a' && c <= 'z') lo[half] = 1;
        else if (c >= 'A' && c <= 'Z') up[half] = 1;
    }
    if (fits && !(up[0] && lo[0]) && !(up[1] && lo[1])) {
        for (int i = 0; i < blen; i++) sfn[i] = l1_upper((uint8_t)name[i]);
        for (int i = 0; i < elen; i++) sfn[8 + i] = l1_upper((uint8_t)dot[1 + i]);
        if (lo[0]) *nt |= FAT32_NT_LOWER_BASE;
        if (lo[1]) *nt |= FAT32_NT_LOWER_EXT;
        return 0;
    }

    /* Basis name (Windows rules, simplified): drop spaces and leading
     * dots, extension = after the last dot, illegal chars -> '_'. */
    const char *p = name;
    while (*p == '.') p++;
    const char *ext = fat32_strrchr(p, '.');
    int b = 0;
    for (const char *s = p; *s && s != ext && b < 8; s++) {
        uint8_t c = (uint8_t)*s;
        if (c == ' ' || c == '.') continue;
        sfn[b++] = sfn_char_ok(c) ? l1_upper(c) : '_';
    }
    if (b == 0) sfn[0] = '_';
    if (ext) {
        int e = 0;
        for (const char *s = ext + 1; *s && e < 3; s++) {
            uint8_t c = (uint8_t)*s;
            if (c == ' ') continue;
            sfn[8 + e++] = sfn_char_ok(c) ? l1_upper(c) : '_';
        }
    }
    return 1;
}

/* Apply a numeric "~N" tail to a basis name: "OCTAMED " -> "OCTAME~1". */
static void fat32_apply_tail(const uint8_t *basis, uint8_t *sfn, uint32_t n)
{
    char tail[9];
    int tl = 0;
    char digits[8];
    int nd = 0;
    do { digits[nd++] = (char)('0' + n % 10); n /= 10; } while (n && nd < 7);
    tail[tl++] = '~';
    while (nd) tail[tl++] = digits[--nd];

    int bl = 0;
    while (bl < 8 && basis[bl] != ' ') bl++;
    if (bl > 8 - tl) bl = 8 - tl;
    memcpy(sfn, basis, 11);
    memset(sfn + bl, ' ', 8 - bl);
    memcpy(sfn + bl, tail, (size_t)tl);
}

/* =========================================================================
 * Directory entry helpers
 * ========================================================================= */

/* Find a directory entry by name within a directory cluster chain.  The
 * name matches an entry's long name or its 8.3 alias, case-insensitively.
 * On success: fills *out_cluster (first cluster), *out_size, *out_attr,
 *   *out_dir_sector (sector containing the 8.3 entry), *out_dir_offset
 *   (byte offset), and leaves the entry's LFN slots in g_lfn_find.
 * Returns 1 on found, 0 on not found. */
static int fat32_find_in_dir(Fat32FS *fs, uint32_t dir_cluster,
                             const char *name,
                             uint32_t *out_cluster, uint32_t *out_size,
                             uint8_t *out_attr,
                             uint32_t *out_dir_sector, uint32_t *out_dir_offset)
{
    uint32_t cluster = dir_cluster;
    uint32_t ents_per_cluster = fs->cluster_size / FAT32_DIR_ENTRY_SIZE;
    uint32_t guard = fs->total_clusters + 2;   /* corrupt-FAT cycle bound */
    Fat32Lfn *l = &g_lfn_find;
    char sname[13];

    lfn_reset(l);
    while (cluster >= 2 && !FAT32_IS_EOC(cluster) && guard-- > 0) {
        if (fat32_read_cluster(fs, cluster, g_cluster_buf) != 0) return 0;
        uint32_t sec = fat32_cluster_to_sector(fs, cluster);

        for (uint32_t e = 0; e < ents_per_cluster; e++) {
            uint8_t *de = &g_cluster_buf[e * FAT32_DIR_ENTRY_SIZE];
            uint32_t esec = sec + (e * FAT32_DIR_ENTRY_SIZE) / fs->bytes_per_sec;
            uint32_t eoff = (e * FAT32_DIR_ENTRY_SIZE) % fs->bytes_per_sec;
            if (de[0] == 0x00) { lfn_reset(l); return 0; }  /* end of directory */
            if (de[0] == 0xE5) { lfn_reset(l); continue; }  /* deleted entry */
            uint8_t attr = de[11];
            if ((attr & 0x0F) == 0x0F) { lfn_feed(l, de, esec, eoff); continue; }
            if ((attr & 0x18) == 0x08 || de[0] == '.') {    /* label, . / .. */
                lfn_reset(l);
                continue;
            }

            int has_lfn = lfn_finish(l, de);
            fat32_83_to_name(de, sname);
            if ((has_lfn && fat32_name_eq(l->name, name)) ||
                fat32_name_eq(sname, name)) {
                if (out_cluster) *out_cluster = le16(&de[26]) | ((uint32_t)le16(&de[20]) << 16);
                if (out_size)    *out_size = le32(&de[28]);
                if (out_attr)    *out_attr = attr;
                if (out_dir_sector) *out_dir_sector = esec;
                if (out_dir_offset) *out_dir_offset = eoff;
                return 1;
            }
            l->nslots = 0;
        }
        cluster = fat32_get_fat_entry(fs, cluster);
    }
    lfn_reset(l);
    return 0;
}

/* Does an entry with exactly these 11 name bytes exist in the directory? */
static int fat32_sfn_exists(Fat32FS *fs, uint32_t dir_cluster, const uint8_t *sfn)
{
    uint32_t cluster = dir_cluster;
    uint32_t ents_per_cluster = fs->cluster_size / FAT32_DIR_ENTRY_SIZE;
    uint32_t guard = fs->total_clusters + 2;

    while (cluster >= 2 && !FAT32_IS_EOC(cluster) && guard-- > 0) {
        if (fat32_read_cluster(fs, cluster, g_cluster_buf) != 0) return 1;
        for (uint32_t e = 0; e < ents_per_cluster; e++) {
            uint8_t *de = &g_cluster_buf[e * FAT32_DIR_ENTRY_SIZE];
            if (de[0] == 0x00) return 0;
            if (de[0] == 0xE5 || (de[11] & 0x0F) == 0x0F) continue;
            if (memcmp(de, sfn, 11) == 0) return 1;
        }
        cluster = fat32_get_fat_entry(fs, cluster);
    }
    return 0;
}

/* Find `count` consecutive free slots (0x00 / 0xE5) in a directory — a
 * run may cross a cluster boundary.  Extends the directory as needed.
 * Returns 1 and fills out[0..count-1] on success, 0 on failure. */
static int fat32_find_free_run(Fat32FS *fs, uint32_t dir_cluster,
                               int count, Fat32Slot *out)
{
    uint32_t cluster = dir_cluster;
    uint32_t ents_per_cluster = fs->cluster_size / FAT32_DIR_ENTRY_SIZE;
    uint32_t last_cluster = cluster;
    uint32_t guard = fs->total_clusters + 2;   /* corrupt-FAT cycle bound */
    int run = 0;

    while (cluster >= 2 && !FAT32_IS_EOC(cluster) && guard-- > 0) {
        if (fat32_read_cluster(fs, cluster, g_cluster_buf) != 0) return 0;
        uint32_t sec = fat32_cluster_to_sector(fs, cluster);

        for (uint32_t e = 0; e < ents_per_cluster; e++) {
            uint8_t *de = &g_cluster_buf[e * FAT32_DIR_ENTRY_SIZE];
            if (de[0] == 0x00 || de[0] == 0xE5) {
                out[run].sec = sec + (e * FAT32_DIR_ENTRY_SIZE) / fs->bytes_per_sec;
                out[run].off = (e * FAT32_DIR_ENTRY_SIZE) % fs->bytes_per_sec;
                if (++run == count) return 1;
            } else {
                run = 0;
            }
        }
        last_cluster = cluster;
        cluster = fat32_get_fat_entry(fs, cluster);
    }

    /* Directory is full — extend; fat32_alloc_cluster zeroes new clusters,
     * so every slot in them is free (0x00 = end). */
    while (run < count) {
        uint32_t newc = fat32_extend_chain(fs, last_cluster);
        if (newc == 0) return 0;
        uint32_t sec = fat32_cluster_to_sector(fs, newc);
        for (uint32_t e = 0; e < ents_per_cluster && run < count; e++, run++) {
            out[run].sec = sec + (e * FAT32_DIR_ENTRY_SIZE) / fs->bytes_per_sec;
            out[run].off = (e * FAT32_DIR_ENTRY_SIZE) % fs->bytes_per_sec;
        }
        last_cluster = newc;
    }
    return 1;
}

/* Write a 32-byte directory entry at a specific sector+offset */
static int fat32_write_dir_entry(Fat32FS *fs,
                                 uint32_t sector, uint32_t offset,
                                 const uint8_t *entry)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        if (BlockDev_Read(fs->bdev, sector, g_sector_buf, 1) != 0) continue;
        memcpy(&g_sector_buf[offset], entry, FAT32_DIR_ENTRY_SIZE);
        if (fat32_bwrite(fs->bdev, sector, g_sector_buf, 1) != 0) continue;
        if (BlockDev_Read(fs->bdev, sector, g_sector_buf2, 1) != 0) continue;
        if (memcmp(&g_sector_buf2[offset], entry, FAT32_DIR_ENTRY_SIZE) == 0)
            return 0;
    }
    printf("[FAT32] Directory entry write did not persist at sector %u\n", sector);
    return -1;
}

/* Build a raw 32-byte directory entry (name left blank for
 * fat32_add_entry to fill in). */
static void fat32_build_dir_entry(uint8_t *de, uint8_t attr, uint32_t cluster,
                                  uint32_t size)
{
    memset(de, 0, FAT32_DIR_ENTRY_SIZE);
    memset(de, ' ', 11);
    de[11] = attr;
    put_le16(&de[20], (uint16_t)(cluster >> 16));    /* fst_clus_hi */
    put_le16(&de[26], (uint16_t)(cluster & 0xFFFF)); /* fst_clus_lo */
    put_le32(&de[28], size);                          /* file_size */
}

/* Add a directory entry named `name` to dir_cluster.  tmpl supplies every
 * field but the name (attr, cluster, size, timestamps).  A plain 8.3 name
 * is written as a single entry; anything else gets a unique "~N" alias
 * plus the LFN chain.  The 8.3 slot is returned via out_sec/out_off.
 * Returns 0 on success, -1 on failure. */
static int fat32_add_entry(Fat32FS *fs, uint32_t dir_cluster, const char *name,
                           const uint8_t *tmpl,
                           uint32_t *out_sec, uint32_t *out_off)
{
    uint8_t sfn[11], basis[11], nt = 0;
    int need_lfn = fat32_make_sfn(name, sfn, &nt);
    if (need_lfn < 0) return -1;
    if (need_lfn) {
        uint32_t n;
        memcpy(basis, sfn, 11);
        for (n = 1; n < 1000000; n++) {
            fat32_apply_tail(basis, sfn, n);
            if (!fat32_sfn_exists(fs, dir_cluster, sfn)) break;
        }
        if (n == 1000000) return -1;
    } else if (fat32_sfn_exists(fs, dir_cluster, sfn)) {
        return -1;
    }

    int len = (int)strlen(name);
    int nl = need_lfn ? (len + 12) / 13 : 0;
    Fat32Slot slots[FAT32_LFN_ENTS_MAX + 1];
    if (!fat32_find_free_run(fs, dir_cluster, nl + 1, slots)) return -1;

    uint8_t chk = fat32_lfn_checksum(sfn);
    uint8_t de[FAT32_DIR_ENTRY_SIZE];
    for (int i = 0; i < nl; i++) {
        int seq = nl - i;                       /* on disk: last fragment first */
        memset(de, 0, sizeof(de));
        de[0] = (uint8_t)(seq | (i == 0 ? FAT32_LFN_LAST : 0));
        de[11] = FAT32_ATTR_LONG_NAME;
        de[13] = chk;
        for (int k = 0; k < 13; k++) {
            int pos = (seq - 1) * 13 + k;
            uint16_t c = pos < len ? (uint8_t)name[pos] : pos == len ? 0x0000 : 0xFFFF;
            put_le16(&de[g_lfn_offs[k]], c);
        }
        if (fat32_write_dir_entry(fs, slots[i].sec, slots[i].off, de) != 0)
            return -1;
    }

    memcpy(de, tmpl, sizeof(de));
    memcpy(de, sfn, 11);
    de[12] = nt;
    if (fat32_write_dir_entry(fs, slots[nl].sec, slots[nl].off, de) != 0)
        return -1;
    *out_sec = slots[nl].sec;
    *out_off = slots[nl].off;
    return 0;
}

/* Set the first byte of a directory slot to 0xE5 (deleted). */
static int fat32_mark_deleted(Fat32FS *fs, uint32_t sec, uint32_t off)
{
    if (BlockDev_Read(fs->bdev, sec, g_sector_buf, 1) != 0) return -1;
    g_sector_buf[off] = 0xE5;
    return fat32_bwrite(fs->bdev, sec, g_sector_buf, 1) != 0 ? -1 : 0;
}

/* Delete an 8.3 entry together with its LFN chain. */
static int fat32_remove_entry(Fat32FS *fs, uint32_t sec, uint32_t off,
                              const Fat32Slot *lfn, int nlfn)
{
    int rc = fat32_mark_deleted(fs, sec, off);
    for (int i = 0; i < nlfn; i++)
        rc |= fat32_mark_deleted(fs, lfn[i].sec, lfn[i].off);
    return rc;
}

/* =========================================================================
 * FAT timestamps (UAOS-29)
 *
 * FAT stores a packed local date/time: date = (year-1980)<<9 | month<<5 |
 * day, time = hour<<11 | minute<<5 | sec/2.  Amiga DateStamp counts days
 * since 1978-01-01 plus minutes and 1/50s ticks. */
static void fat32_now(uint16_t *fat_date, uint16_t *fat_time)
{
    RtcDateTime t = RTC_ReadDateTime();
    int year = t.year;
    if (year < 1980) year = 1980;
    if (year > 2107) year = 2107;
    *fat_date = (uint16_t)(((year - 1980) << 9) | (t.month << 5) | t.day);
    *fat_time = (uint16_t)((t.hour << 11) | (t.min << 5) | (t.sec >> 1));
}

/* days since 1970-01-01 from a civil date (Howard Hinnant's algorithm) */
static int32_t fat32_days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? 9 : -3)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

static void fat32_civil_from_days(int32_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int era = (int)((z >= 0 ? z : z - 146096) / 146097);
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int yr = (int)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = yr + (*m <= 2);
}

/* Amiga DateStamp -> FAT date/time.  Returns 0 if the date precedes the
 * FAT epoch (1980) — the fields are left zeroed then. */
static void fat32_ds_to_fatstamp(int32_t days, int32_t mins, int32_t ticks,
                                 uint16_t *fat_date, uint16_t *fat_time)
{
    /* Amiga epoch 1978-01-01 -> unix days, then to a civil date. */
    const int32_t unix_days = days - 2922;
    int y; unsigned m, d;
    fat32_civil_from_days(unix_days, &y, &m, &d);
    uint32_t secs = (uint32_t)mins * 60 + (uint32_t)ticks / 50;
    if (y < 1980) { *fat_date = 0; *fat_time = 0; return; }
    if (y > 2107) y = 2107;
    *fat_date = (uint16_t)(((y - 1980) << 9) | (m << 5) | d);
    *fat_time = (uint16_t)(((secs / 3600) << 11) | (((secs / 60) % 60) << 5) |
                           ((secs % 60) >> 1));
}

/* FAT date/time -> Amiga DateStamp fields (ds_Days/ds_Minute/ds_Tick). */
static void fat32_fatstamp_to_ds(uint16_t fat_date, uint16_t fat_time,
                                 int32_t *days, int32_t *mins, int32_t *ticks)
{
    const int y = 1980 + (fat_date >> 9);
    const unsigned m = (fat_date >> 5) & 0xF;
    const unsigned d = fat_date & 0x1F;
    if (!fat_date || !m || !d) { *days = 0; *mins = 0; *ticks = 0; return; }
    const int32_t unix_days = fat32_days_from_civil(y, m, d);
    *days = unix_days + 2922;
    const unsigned h = fat_time >> 11;
    const unsigned mi = (fat_time >> 5) & 0x3F;
    const unsigned s = (fat_time & 0x1F) * 2;
    *mins = (int32_t)(h * 60 + mi);
    *ticks = (int32_t)(s * 50);
}

/* Stamp a freshly built entry (create/write times + access date). */
static void fat32_stamp_entry(uint8_t *de, int created)
{
    uint16_t fd, ft;
    fat32_now(&fd, &ft);
    put_le16(&de[22], ft);    /* wrt_time  */
    put_le16(&de[24], fd);    /* wrt_date  */
    put_le16(&de[18], fd);    /* lst_acc_date */
    if (created) {
        put_le16(&de[14], ft);/* crt_time  */
        put_le16(&de[16], fd);/* crt_date  */
    }
}

/* =========================================================================
 * Path parsing — strip volume prefix and walk directory tree
 * ========================================================================= */

/* Skip the "VOL:" prefix from a path, returning a pointer to the rest.
 * If no colon, return the path as-is. */
static const char *strip_vol_prefix(const char *path)
{
    const char *p = path;
    while (*p && *p != ':') p++;
    if (*p == ':') return p + 1;
    return path;
}

/* Copy a relative path into buf[128] with trailing slashes removed. */
static char *fat32_copy_path(const char *path, char *buf)
{
    int i = 0;
    while (i < 127 && path[i]) { buf[i] = path[i]; i++; }
    while (i > 0 && (buf[i - 1] == '/' || buf[i - 1] == '\\')) i--;
    buf[i] = '\0';
    return buf;
}

/* Final component of a relative path ("a/b/Long Name" -> "Long Name"). */
static const char *fat32_basename(const char *rel, char *buf)
{
    char *base = fat32_copy_path(rel, buf);
    for (char *s = buf; *s; s++)
        if (*s == '/' || *s == '\\') base = s + 1;
    return base;
}

/* Walk a path like "dir1/dir2/file" from a starting directory cluster.
 * For each component except the last, descend into the directory.
 * Components match long names or 8.3 aliases (fat32_find_in_dir).
 * On success: fills *out_dir_cluster (parent dir cluster) and optionally
 *   the entry info.  *out_dir_cluster is also set when only the final
 *   component is missing, so callers can create it there.
 * Returns 1 on success (final component found), 0 on not found,
 *   -1 on error (intermediate dir not found). */
static int fat32_walk_path(Fat32FS *fs, const char *path,
                           uint32_t start_cluster,
                           uint32_t *out_dir_cluster,
                           uint32_t *out_file_cluster,
                           uint32_t *out_file_size,
                           uint8_t *out_file_attr,
                           uint32_t *out_entry_sector,
                           uint32_t *out_entry_offset)
{
    /* Copy path to a local buffer for tokenization */
    char tmp[128];
    fat32_copy_path(path, tmp);

    /* Strip leading slashes */
    char *p = tmp;
    while (*p == '/' || *p == '\\') p++;

    uint32_t dir_cluster = start_cluster;
    char *next_slash;

    while (1) {
        /* Find the next slash */
        next_slash = p;
        while (*next_slash && *next_slash != '/' && *next_slash != '\\')
            next_slash++;

        if (*next_slash == '\0') {
            /* Final component */
            if (out_dir_cluster) *out_dir_cluster = dir_cluster;
            if (*p == '\0') return 1;  /* empty path = root directory itself */
            return fat32_find_in_dir(fs, dir_cluster, p,
                                     out_file_cluster, out_file_size,
                                     out_file_attr,
                                     out_entry_sector, out_entry_offset);
        }

        /* Intermediate component — must be a directory */
        *next_slash = '\0';
        uint32_t sub_cluster = 0;
        uint8_t sub_attr = 0;
        int found = fat32_find_in_dir(fs, dir_cluster, p,
                                      &sub_cluster, NULL, &sub_attr,
                                      NULL, NULL);
        if (!found || !(sub_attr & FAT32_ATTR_DIRECTORY))
            return -1;  /* intermediate directory not found */
        dir_cluster = sub_cluster;
        p = next_slash + 1;
        while (*p == '/' || *p == '\\') p++;
    }
}

/* =========================================================================
 * Mount / Unmount
 * ========================================================================= */

Fat32FS *FAT32_Mount(BlockDev *bdev)
{
    if (!bdev) {
        printf("[FAT32] Invalid block device\n");
        return NULL;
    }

    Fat32FS *fs = &g_fat32_fs;
    memset(fs, 0, sizeof(Fat32FS));
    g_dir_buf_cluster = 0;
    fs->bdev = bdev;

    /* Read boot sector using the DMA-safe sector buffer */
    if (BlockDev_Read(bdev, 0, g_sector_buf, 1) != 0) {
        printf("[FAT32] Failed to read boot sector\n");
        return NULL;
    }

    memcpy(&fs->bpb, g_sector_buf, sizeof(Fat32BPB));

    /* Validate FAT32 signature */
    if (fs->bpb.boot_sig55aa != 0xAA55) {
        printf("[FAT32] Invalid boot signature\n");
        return NULL;
    }

    /* Validate that this is actually a FAT32 BPB — a bare 0x55AA check
     * also accepts FAT12/16 and exFAT boot sectors, which would then
     * "mount" with garbage geometry and fail every operation.
     *   - exFAT:        OEM name is "EXFAT   " and BPB fields are 0
     *   - FAT12/16:     root_ent_cnt != 0 and fat_sz32 == 0
     *   - FAT32:        root_ent_cnt == 0, fat_sz32 != 0, root_clus >= 2 */
    if (memcmp(fs->bpb.oem_name, "EXFAT   ", 8) == 0) {
        printf("[FAT32] exFAT volume is not supported\n");
        return NULL;
    }
    if (fs->bpb.root_ent_cnt != 0 || fs->bpb.fat_sz16 != 0) {
        printf("[FAT32] FAT12/FAT16 volume is not supported\n");
        return NULL;
    }

    /* Parse BPB */
    fs->bytes_per_sec = le16((uint8_t*)&fs->bpb.bytes_per_sec);
    fs->sec_per_clus = fs->bpb.sec_per_clus;
    fs->cluster_size = fs->bytes_per_sec * fs->sec_per_clus;
    fs->root_cluster = le32((uint8_t*)&fs->bpb.root_clus);

    if (fs->bytes_per_sec != 512 || fs->sec_per_clus == 0 ||
        (fs->sec_per_clus & (fs->sec_per_clus - 1)) != 0) {
        printf("[FAT32] Unsupported sector/cluster geometry\n");
        return NULL;
    }
    if (fs->bpb.fat_sz32 == 0 || fs->bpb.num_fats == 0 || fs->root_cluster < 2) {
        printf("[FAT32] Invalid FAT32 BPB fields\n");
        return NULL;
    }
    if (fs->cluster_size > sizeof(g_cluster_buf) ||
        fs->sec_per_clus > FAT32_MAX_CLUSTER_SECS) {
        printf("[FAT32] Cluster size too large for I/O buffers\n");
        return NULL;
    }

    /* Calculate FAT and data start sectors */
    uint32_t rsvd_sec = le16((uint8_t*)&fs->bpb.rsvd_sec_cnt);
    uint32_t num_fats = fs->bpb.num_fats;
    uint32_t fat_sz32 = le32((uint8_t*)&fs->bpb.fat_sz32);

    fs->fat_start = rsvd_sec;
    fs->data_start = rsvd_sec + (num_fats * fat_sz32);

    /* Calculate total data clusters */
    uint32_t tot_sec = le32((uint8_t*)&fs->bpb.tot_sec32);
    if (tot_sec == 0) tot_sec = le16((uint8_t*)&fs->bpb.tot_sec16);
    if (tot_sec <= fs->data_start) {
        printf("[FAT32] BPB geometry inconsistent (tot_sec=%u, data_start=%u)\n",
               tot_sec, fs->data_start);
        return NULL;
    }
    fs->total_clusters = (tot_sec - fs->data_start) / fs->sec_per_clus;
    /* Clamp to what the FAT can actually address: the FAT holds
     * fat_sz32 * (bytes_per_sec/4) entries, of which 0 and 1 are
     * reserved.  Volumes whose BPB claims more clusters than the FAT
     * can hold (e.g. written by a non-converged formatter) would
     * otherwise report the tail clusters as permanently "used" and
     * could walk past the FAT end. */
    uint32_t fat_capacity = fat_sz32 * (fs->bytes_per_sec / 4);
    if (fat_capacity > 2 && fs->total_clusters > fat_capacity - 2)
        fs->total_clusters = fat_capacity - 2;
    if (fs->total_clusters == 0) {
        printf("[FAT32] No data clusters\n");
        return NULL;
    }
    fs->free_clusters = -1;   /* computed lazily by FAT32_GetVolumeStats */

    printf("[FAT32] Mounted: bytes/sec=%u, sec/clus=%u, root_clus=%u, "
           "total_clusters=%u\n",
           fs->bytes_per_sec, fs->sec_per_clus, fs->root_cluster,
           fs->total_clusters);

    return fs;
}

void FAT32_Unmount(Fat32FS *fs)
{
    (void)fs;
}

/* =========================================================================
 * File operations
 * ========================================================================= */

Fat32File *FAT32_Open(Fat32FS *fs, const char *path)
{
    if (!fs || !path) return NULL;
    g_dir_buf_cluster = 0;   /* a fresh lock re-reads its directory */

    const char *rel = strip_vol_prefix(path);
    /* Empty path or "." = root directory */
    if (*rel == '\0' || (*rel == '.' && rel[1] == '\0')) {
        Fat32File *f = fat32_alloc_file();
        if (!f) return NULL;
        f->fs = fs;
        f->start_cluster = fs->root_cluster;
        f->cluster = fs->root_cluster;
        f->offset = 0;
        f->pos = 0;
        f->size = 0;
        f->is_dir = 1;
        return f;
    }

    uint32_t file_cluster = 0, file_size = 0;
    uint8_t file_attr = 0;
    uint32_t entry_sec = 0, entry_off = 0;
    uint32_t dir_cluster = 0;

    int found = fat32_walk_path(fs, rel, fs->root_cluster,
                                &dir_cluster,
                                &file_cluster, &file_size, &file_attr,
                                &entry_sec, &entry_off);
    if (found != 1) return NULL;

    Fat32File *f = fat32_alloc_file();
    if (!f) return NULL;
    f->fs = fs;
    f->start_cluster = file_cluster;
    f->cluster = file_cluster;
    f->offset = 0;
    f->pos = 0;
    f->size = file_size;
    f->is_dir = (file_attr & FAT32_ATTR_DIRECTORY) ? 1 : 0;
    f->dir_sector = entry_sec;
    f->dir_offset = entry_off;
    f->parent_cluster = dir_cluster;
    return f;
}

Fat32File *FAT32_CreateFile(Fat32FS *fs, const char *path)
{
    if (!fs || !path) return NULL;
    const char *rel = strip_vol_prefix(path);
    if (*rel == '\0') return NULL;

    /* Check if file already exists */
    uint32_t existing_cluster = 0, existing_size = 0;
    uint8_t existing_attr = 0;
    uint32_t entry_sec = 0, entry_off = 0;
    uint32_t dir_cluster = 0;

    int found = fat32_walk_path(fs, rel, fs->root_cluster,
                                &dir_cluster,
                                &existing_cluster, &existing_size,
                                &existing_attr, &entry_sec, &entry_off);

    if (found == 1) {
        /* File exists — if it's a directory, can't truncate */
        if (existing_attr & FAT32_ATTR_DIRECTORY) return NULL;
        /* Free existing cluster chain and reset entry */
        if (existing_cluster >= 2)
            fat32_free_chain(fs, existing_cluster);
        /* Truncate in place: cluster=0, size=0, fresh timestamps.  The
         * 8.3 name bytes stay untouched so an LFN chain's checksum still
         * binds to this entry. */
        uint8_t entry[32];
        if (BlockDev_Read(fs->bdev, entry_sec, g_sector_buf, 1) != 0) return NULL;
        memcpy(entry, &g_sector_buf[entry_off], sizeof(entry));
        put_le16(&entry[20], 0);
        put_le16(&entry[26], 0);
        put_le32(&entry[28], 0);
        fat32_stamp_entry(entry, 1);
        fat32_write_dir_entry(fs, entry_sec, entry_off, entry);
    } else if (found == 0) {
        /* File doesn't exist — walk_path left the parent in dir_cluster */
        char nbuf[128];
        uint8_t entry[32];
        fat32_build_dir_entry(entry, 0, 0, 0);
        fat32_stamp_entry(entry, 1);
        if (fat32_add_entry(fs, dir_cluster, fat32_basename(rel, nbuf), entry,
                            &entry_sec, &entry_off) != 0)
            return NULL;
    } else {
        return NULL;  /* intermediate dir not found */
    }

    Fat32File *f = fat32_alloc_file();
    if (!f) return NULL;
    f->fs = fs;
    f->start_cluster = 0;  /* empty file */
    f->cluster = 0;
    f->offset = 0;
    f->pos = 0;
    f->size = 0;
    f->is_dir = 0;
    f->dir_sector = entry_sec;
    f->dir_offset = entry_off;
    f->parent_cluster = dir_cluster;
    return f;
}

void FAT32_Close(Fat32File *file)
{
    if (!file) return;
    /* Flush file size to the directory entry if this is a file */
    if (file->fs && !file->is_dir && file->dir_sector != 0) {
        /* Read the directory entry, update the size field, write back */
        if (BlockDev_Read(file->fs->bdev, file->dir_sector,
                          g_sector_buf, 1) == 0) {
            put_le32(&g_sector_buf[file->dir_offset + 28], file->size);
            put_le16(&g_sector_buf[file->dir_offset + 20],
                     (uint16_t)(file->start_cluster >> 16));
            put_le16(&g_sector_buf[file->dir_offset + 26],
                     (uint16_t)(file->start_cluster & 0xFFFF));
            fat32_stamp_entry(&g_sector_buf[file->dir_offset], 0);
            fat32_bwrite(file->fs->bdev, file->dir_sector,
                           g_sector_buf, 1);
        }
    }
    file->in_use = 0;
}

uint32_t FAT32_Read(Fat32File *file, void *buffer, uint32_t len)
{
    if (!file || !file->fs || file->is_dir) return 0;
    if (file->pos >= file->size) return 0;

    uint32_t remaining = file->size - file->pos;
    if (len > remaining) len = remaining;

    uint8_t *buf = (uint8_t *)buffer;
    uint32_t n = 0;
    uint32_t cluster_size = file->fs->cluster_size;

    while (n < len) {
        if (file->cluster < 2 || FAT32_IS_EOC(file->cluster)) break;

        /* g_cluster_buf is shared across all files and dir scans, so it
         * cannot be trusted to hold file->cluster's data (e.g. after a
         * mid-cluster seek).  Re-read the cluster unconditionally. */
        if (fat32_read_cluster(file->fs, file->cluster,
                               g_cluster_buf) != 0) break;

        uint32_t chunk = cluster_size - file->offset;
        if (chunk > len - n) chunk = len - n;

        memcpy(buf + n, g_cluster_buf + file->offset, chunk);
        n += chunk;
        file->offset += chunk;
        file->pos += chunk;

        /* Advance to next cluster if we've consumed this one */
        if (file->offset >= cluster_size) {
            file->cluster = fat32_get_fat_entry(file->fs, file->cluster);
            file->offset = 0;
        }
    }
    return n;
}

uint32_t FAT32_Write(Fat32File *file, const void *buffer, uint32_t len)
{
    if (!file || !file->fs || file->is_dir) return 0;
    const uint8_t *buf = (const uint8_t *)buffer;
    uint32_t cluster_size = file->fs->cluster_size;
    uint32_t n = 0;

    /* If file has no clusters yet, allocate the first one */
    if (file->start_cluster == 0) {
        uint32_t c = fat32_alloc_cluster(file->fs);
        if (c == 0) return 0;
        file->start_cluster = c;
        file->cluster = c;
        file->offset = 0;
    }

    while (n < len) {
        /* If we've passed the end of the current cluster, allocate a new one */
        if (file->offset >= cluster_size) {
            uint32_t next = fat32_get_fat_entry(file->fs, file->cluster);
            if (FAT32_IS_EOC(next) || next < 2) {
                next = fat32_extend_chain(file->fs, file->cluster);
                if (next == 0) break;
            }
            file->cluster = next;
            file->offset = 0;
        }

        /* Read the current cluster (to preserve unwritten parts) */
        if (fat32_read_cluster(file->fs, file->cluster, g_cluster_buf) != 0)
            break;

        uint32_t chunk = cluster_size - file->offset;
        if (chunk > len - n) chunk = len - n;

        memcpy(g_cluster_buf + file->offset, buf + n, chunk);

        /* Write the cluster back */
        if (fat32_write_cluster(file->fs, file->cluster, g_cluster_buf) != 0)
            break;

        n += chunk;
        file->offset += chunk;
        file->pos += chunk;
        if (file->pos > file->size)
            file->size = file->pos;
    }
    return n;
}

void FAT32_Seek(Fat32File *file, uint32_t pos)
{
    if (!file) return;
    /* For files: walk the cluster chain to the right position */
    if (!file->is_dir && file->start_cluster >= 2) {
        uint32_t cluster_size = file->fs->cluster_size;
        uint32_t target_cluster = pos / cluster_size;
        uint32_t cluster = file->start_cluster;
        for (uint32_t i = 0; i < target_cluster; i++) {
            uint32_t next = fat32_get_fat_entry(file->fs, cluster);
            if (FAT32_IS_EOC(next) || next < 2) break;
            cluster = next;
        }
        file->cluster = cluster;
        file->offset = pos % cluster_size;
    }
    file->pos = pos;
}

uint32_t FAT32_Size(Fat32File *file)
{
    if (!file) return 0;
    return file->size;
}

/* =========================================================================
 * Directory iteration
 * ========================================================================= */

int FAT32_ReadDir(Fat32File *dir, char *name, int name_max,
                  uint32_t *size, uint8_t *is_dir,
                  uint16_t *wrt_time, uint16_t *wrt_date)
{
    if (!dir || !dir->fs || !dir->is_dir || !name || name_max < 2) return 0;
    Fat32FS *fs = dir->fs;
    uint32_t cluster_size = fs->cluster_size;
    uint32_t ents_per_cluster = cluster_size / FAT32_DIR_ENTRY_SIZE;
    Fat32Lfn *l = &g_lfn_iter;

    /* Initialize iterator on first call (iter_cluster == 0 means start) */
    if (dir->iter_cluster == 0) {
        dir->iter_cluster = dir->start_cluster;
        dir->iter_offset = 0;
    }

    /* Each call ends right after an 8.3 entry, so an LFN chain never
     * straddles two calls — the accumulator starts clean every time. */
    lfn_reset(l);
    uint32_t guard = fs->total_clusters + 2;   /* corrupt-FAT cycle bound */
    while (dir->iter_cluster >= 2 && !FAT32_IS_EOC(dir->iter_cluster) &&
           guard-- > 0) {
        if (g_dir_buf_cluster != dir->iter_cluster) {
            if (fat32_read_cluster(fs, dir->iter_cluster, g_dir_buf) != 0) {
                g_dir_buf_cluster = 0;
                return 0;
            }
            g_dir_buf_cluster = dir->iter_cluster;
        }

        /* Scan entries in the current cluster from iter_offset */
        uint32_t start_ent = dir->iter_offset / FAT32_DIR_ENTRY_SIZE;
        for (uint32_t e = start_ent; e < ents_per_cluster; e++) {
            uint8_t *de = &g_dir_buf[e * FAT32_DIR_ENTRY_SIZE];

            /* Advance iterator past this entry */
            dir->iter_offset = (e + 1) * FAT32_DIR_ENTRY_SIZE;

            if (de[0] == 0x00) {
                /* End of directory */
                dir->iter_cluster = 0;
                return 0;
            }
            if (de[0] == 0xE5) { lfn_reset(l); continue; }  /* deleted */
            uint8_t attr = de[11];
            if ((attr & 0x0F) == 0x0F) { lfn_feed(l, de, 0, 0); continue; }
            /* Skip volume label and "." / ".." entries */
            if ((attr & 0x18) == 0x08 || de[0] == '.') { lfn_reset(l); continue; }

            /* Found a valid entry.  A long name that doesn't fit the
             * caller's buffer falls back to the 8.3 alias, which still
             * opens the same object. */
            char sname[13];
            const char *src = sname;
            fat32_83_to_name(de, sname);
            if (lfn_finish(l, de) && (int)strlen(l->name) < name_max)
                src = l->name;
            int i = 0;
            while (i < name_max - 1 && src[i]) { name[i] = src[i]; i++; }
            name[i] = '\0';

            if (size)   *size = le32(&de[28]);
            if (is_dir) *is_dir = (attr & FAT32_ATTR_DIRECTORY) ? 1 : 0;
            if (wrt_time) *wrt_time = le16(&de[22]);
            if (wrt_date) *wrt_date = le16(&de[24]);
            return 1;
        }

        /* Move to next cluster in the directory chain */
        dir->iter_cluster = fat32_get_fat_entry(fs, dir->iter_cluster);
        dir->iter_offset = 0;
    }

    dir->iter_cluster = 0;
    return 0;
}

/* =========================================================================
 * Create directory
 * ========================================================================= */

int FAT32_CreateDir(Fat32FS *fs, const char *path)
{
    if (!fs || !path) return -1;
    const char *rel = strip_vol_prefix(path);
    if (*rel == '\0') return -1;

    /* Walk path: the final component must be missing; walk_path then
     * leaves its parent directory in parent_cluster. */
    uint32_t parent_cluster = 0;
    if (fat32_walk_path(fs, rel, fs->root_cluster, &parent_cluster,
                        NULL, NULL, NULL, NULL, NULL) != 0)
        return -1;  /* already exists, or parent missing */
    char nbuf[128];
    const char *name = fat32_basename(rel, nbuf);
    uint8_t sfn_chk[11], nt_chk;
    if (fat32_make_sfn(name, sfn_chk, &nt_chk) < 0) return -1;

    /* Allocate a cluster for the new directory */
    uint32_t new_cluster = fat32_alloc_cluster(fs);
    if (new_cluster == 0) return -1;

    /* Write "." and ".." entries into the new directory cluster.
     * Use the DMA-safe g_cluster_buf instead of a stack buffer — the
     * VirtIO driver requires DMA-accessible buffers, and stack buffers
     * are not guaranteed to be 4K-aligned. */
    memset(g_cluster_buf, 0, fs->sec_per_clus * fs->bytes_per_sec);

    /* "." entry — points to self (name = ".          ") */
    uint8_t *dot_entry = g_cluster_buf;
    memset(dot_entry, ' ', 11);
    dot_entry[0] = '.';
    dot_entry[11] = FAT32_ATTR_DIRECTORY;
    put_le16(&dot_entry[20], (uint16_t)(new_cluster >> 16));
    put_le16(&dot_entry[26], (uint16_t)(new_cluster & 0xFFFF));
    put_le32(&dot_entry[28], 0);

    /* ".." entry — points to parent (name = "..         ").  The FAT
     * spec (and fsck.fat) require cluster 0 when the parent is the root. */
    uint32_t dotdot_cluster = (parent_cluster == fs->root_cluster) ? 0 : parent_cluster;
    uint8_t *dotdot_entry = g_cluster_buf + 32;
    memset(dotdot_entry, ' ', 11);
    dotdot_entry[0] = '.';
    dotdot_entry[1] = '.';
    dotdot_entry[11] = FAT32_ATTR_DIRECTORY;
    put_le16(&dotdot_entry[20], (uint16_t)(dotdot_cluster >> 16));
    put_le16(&dotdot_entry[26], (uint16_t)(dotdot_cluster & 0xFFFF));
    put_le32(&dotdot_entry[28], 0);
    fat32_stamp_entry(dot_entry, 1);
    fat32_stamp_entry(dotdot_entry, 1);

    /* Write the directory cluster — write all sectors at once using
     * the DMA-safe g_cluster_buf. */
    uint32_t dir_sec = fat32_cluster_to_sector(fs, new_cluster);
    if (fat32_bwrite(fs->bdev, dir_sec, g_cluster_buf, fs->sec_per_clus) != 0) {
        printf("[FAT32] Failed to write new directory cluster\n");
        return -1;
    }

    /* Add the entry (plus LFN chain) to the parent directory */
    uint32_t entry_sec = 0, entry_off = 0;
    uint8_t entry[32];
    fat32_build_dir_entry(entry, FAT32_ATTR_DIRECTORY, new_cluster, 0);
    fat32_stamp_entry(entry, 1);
    if (fat32_add_entry(fs, parent_cluster, name, entry,
                        &entry_sec, &entry_off) != 0) {
        fat32_free_chain(fs, new_cluster);
        return -1;
    }

    return 0;
}

/* =========================================================================
 * Delete file or empty directory
 * ========================================================================= */

int FAT32_Delete(Fat32FS *fs, const char *path)
{
    if (!fs || !path) return -1;
    const char *rel = strip_vol_prefix(path);
    if (*rel == '\0') return -1;

    uint32_t file_cluster = 0, file_size = 0;
    uint8_t file_attr = 0;
    uint32_t entry_sec = 0, entry_off = 0;

    int found = fat32_walk_path(fs, rel, fs->root_cluster,
                                NULL,
                                &file_cluster, &file_size, &file_attr,
                                &entry_sec, &entry_off);
    if (found != 1) return -1;

    /* Snapshot the entry's LFN slots before the emptiness check re-walks */
    Fat32Slot lfn[FAT32_LFN_ENTS_MAX];
    int nlfn = g_lfn_find.nslots;
    memcpy(lfn, g_lfn_find.slots, sizeof(lfn[0]) * (size_t)nlfn);

    /* If it's a directory, check that it's empty (only . and .. entries) */
    if (file_attr & FAT32_ATTR_DIRECTORY) {
        if (file_cluster < 2) return -1;
        Fat32File *dir = FAT32_Open(fs, path);
        if (!dir) return -1;
        char name[13];
        uint32_t size;
        uint8_t is_dir;
        while (FAT32_ReadDir(dir, name, (int)sizeof(name), &size, &is_dir,
                             NULL, NULL)) {
            /* Any entry other than . and .. means non-empty */
            FAT32_Close(dir);
            return -1;
        }
        FAT32_Close(dir);
    }

    /* Free the cluster chain */
    if (file_cluster >= 2)
        fat32_free_chain(fs, file_cluster);

    /* Mark the 8.3 entry and its LFN chain deleted (0xE5) */
    return fat32_remove_entry(fs, entry_sec, entry_off, lfn, nlfn);
}

/* =========================================================================
 * Rename (same directory) — UAOS-29, VFAT long names UAOS-254
 * ========================================================================= */

int FAT32_Rename(Fat32FS *fs, const char *old_path, const char *new_path)
{
    if (!fs || !old_path || !new_path) return -1;
    const char *old_rel = strip_vol_prefix(old_path);
    const char *new_rel = strip_vol_prefix(new_path);
    if (*old_rel == '\0' || *new_rel == '\0') return -1;

    /* Resolve the source entry (parent cluster + slot position). */
    uint32_t old_dir = 0, file_cluster = 0, file_size = 0;
    uint8_t file_attr = 0;
    uint32_t entry_sec = 0, entry_off = 0;

    int found = fat32_walk_path(fs, old_rel, fs->root_cluster,
                                &old_dir,
                                &file_cluster, &file_size, &file_attr,
                                &entry_sec, &entry_off);
    if (found != 1) return -1;

    /* Snapshot the old entry and its LFN chain (raw bytes, so a failed
     * rename can put them back). */
    Fat32Slot lfn[FAT32_LFN_ENTS_MAX];
    static uint8_t lfn_raw[FAT32_LFN_ENTS_MAX][FAT32_DIR_ENTRY_SIZE];
    uint8_t old_de[FAT32_DIR_ENTRY_SIZE];
    int nlfn = g_lfn_find.nslots;
    memcpy(lfn, g_lfn_find.slots, sizeof(lfn[0]) * (size_t)nlfn);
    for (int i = 0; i < nlfn; i++) {
        if (BlockDev_Read(fs->bdev, lfn[i].sec, g_sector_buf, 1) != 0) return -1;
        memcpy(lfn_raw[i], &g_sector_buf[lfn[i].off], FAT32_DIR_ENTRY_SIZE);
    }
    if (BlockDev_Read(fs->bdev, entry_sec, g_sector_buf, 1) != 0) return -1;
    memcpy(old_de, &g_sector_buf[entry_off], sizeof(old_de));

    /* Resolve the destination.  It may only "exist" as the source itself
     * (a case-only rename such as foo -> Foo). */
    uint32_t new_dir = 0, nsec = 0, noff = 0;
    int nfound = fat32_walk_path(fs, new_rel, fs->root_cluster, &new_dir,
                                 NULL, NULL, NULL, &nsec, &noff);
    if (nfound < 0) return -1;
    if (nfound == 1 && (nsec != entry_sec || noff != entry_off)) return -1;

    /* Same-directory rename only — a cross-directory move would have to
     * relocate the entry between clusters; AmigaDOS callers that want a
     * move get copy+delete at a higher level. */
    if (new_dir != old_dir) return -1;

    char nbuf[128];
    const char *new_name = fat32_basename(new_rel, nbuf);
    uint8_t sfn_chk[11], nt_chk;
    if (fat32_make_sfn(new_name, sfn_chk, &nt_chk) < 0) return -1;

    /* Re-create the entry under the new name (new alias + LFN chain as
     * needed); cluster, attributes, size and timestamps travel with it.
     * The old entry goes first so its slots and alias can be reused. */
    if (fat32_remove_entry(fs, entry_sec, entry_off, lfn, nlfn) != 0) return -1;
    uint32_t sec2 = 0, off2 = 0;
    if (fat32_add_entry(fs, old_dir, new_name, old_de, &sec2, &off2) != 0) {
        for (int i = 0; i < nlfn; i++)
            fat32_write_dir_entry(fs, lfn[i].sec, lfn[i].off, lfn_raw[i]);
        fat32_write_dir_entry(fs, entry_sec, entry_off, old_de);
        printf("[FAT32] Rename to '%s' failed\n", new_name);
        return -1;
    }

    /* Open handles locate their entry by slot — follow it to the new one,
     * or FAT32_Close would write size/cluster into a reused slot. */
    for (int i = 0; i < FAT32_MAX_FILES; i++) {
        Fat32File *f = &g_fat32_files[i];
        if (f->in_use && f->fs == fs &&
            f->dir_sector == entry_sec && f->dir_offset == entry_off) {
            f->dir_sector = sec2;
            f->dir_offset = off2;
        }
    }
    return 0;
}

/* =========================================================================
 * Set write timestamp (ACTION_SET_DATE) — UAOS-29
 * ========================================================================= */

int FAT32_SetDate(Fat32FS *fs, const char *path,
                  int32_t days, int32_t mins, int32_t ticks)
{
    if (!fs || !path) return -1;
    const char *rel = strip_vol_prefix(path);
    if (*rel == '\0') return -1;

    uint32_t file_cluster = 0, file_size = 0;
    uint8_t file_attr = 0;
    uint32_t entry_sec = 0, entry_off = 0;
    int found = fat32_walk_path(fs, rel, fs->root_cluster,
                                NULL,
                                &file_cluster, &file_size, &file_attr,
                                &entry_sec, &entry_off);
    if (found != 1) return -1;

    uint16_t fat_date, fat_time;
    fat32_ds_to_fatstamp(days, mins, ticks, &fat_date, &fat_time);

    if (BlockDev_Read(fs->bdev, entry_sec, g_sector_buf, 1) != 0) return -1;
    put_le16(&g_sector_buf[entry_off + 22], fat_time);
    put_le16(&g_sector_buf[entry_off + 24], fat_date);
    if (fat32_bwrite(fs->bdev, entry_sec, g_sector_buf, 1) != 0) return -1;
    return 0;
}

int FAT32_GetDate(Fat32File *file, uint16_t *fat_time, uint16_t *fat_date)
{
    if (!file || !file->fs || !file->dir_sector) return -1;
    if (BlockDev_Read(file->fs->bdev, file->dir_sector,
                      g_sector_buf, 1) != 0) return -1;
    if (fat_time) *fat_time = le16(&g_sector_buf[file->dir_offset + 22]);
    if (fat_date) *fat_date = le16(&g_sector_buf[file->dir_offset + 24]);
    return 0;
}

/* =========================================================================
 * Set file size (ACTION_SET_FILE_SIZE) — UAOS-248
 * ========================================================================= */

int FAT32_SetFileSize(Fat32File *file, uint32_t new_size)
{
    if (!file || !file->fs || file->is_dir) return -1;

    uint32_t old_size = file->size;
    if (new_size > old_size) {
        /* Extend with zeros through the normal write path (allocates
         * clusters as needed).  Keep g_cluster_buf for the FS internals
         * and use a small dedicated zero block. */
        static const uint8_t zeros[512] = {0};
        uint32_t save_pos = file->pos;
        FAT32_Seek(file, old_size);
        uint32_t remaining = new_size - old_size;
        while (remaining) {
            uint32_t chunk = remaining > sizeof(zeros)
                             ? (uint32_t)sizeof(zeros) : remaining;
            if (FAT32_Write(file, zeros, chunk) != chunk) {
                FAT32_Seek(file, save_pos);
                return -1;
            }
            remaining -= chunk;
        }
        FAT32_Seek(file, save_pos);
    } else {
        /* Shrink: adjust the logical size.  Trailing clusters stay
         * allocated — unreachable past EOF — until a later write or
         * truncate reuses them. */
        file->size = new_size;
        if (file->pos > new_size) FAT32_Seek(file, new_size);
    }

    /* Flush the size to the directory entry immediately */
    if (file->dir_sector != 0 &&
        BlockDev_Read(file->fs->bdev, file->dir_sector,
                      g_sector_buf, 1) == 0) {
        put_le32(&g_sector_buf[file->dir_offset + 28], file->size);
        fat32_stamp_entry(&g_sector_buf[file->dir_offset], 0);
        fat32_bwrite(file->fs->bdev, file->dir_sector, g_sector_buf, 1);
    }
    return 0;
}

/* =========================================================================
 * Set protection (ACTION_SET_PROTECT) — UAOS-248
 * FAT has no Amiga-style protection bits; the closest real attribute is
 * ATTR_READ_ONLY, mapped from the Amiga 'w' (write-protected) bit.
 * ========================================================================= */

int FAT32_SetProtection(Fat32FS *fs, const char *path, uint32_t mask)
{
    if (!fs || !path) return -1;
    const char *rel = strip_vol_prefix(path);
    if (*rel == '\0') return -1;

    uint32_t entry_sec = 0, entry_off = 0;
    if (fat32_walk_path(fs, rel, fs->root_cluster,
                        NULL, NULL, NULL, NULL,
                        &entry_sec, &entry_off) != 1)
        return -1;

    if (BlockDev_Read(fs->bdev, entry_sec, g_sector_buf, 1) != 0) return -1;
    uint8_t attr = g_sector_buf[entry_off + 11];
    if (mask & FIBF_WRITE) attr |=  FAT32_ATTR_READ_ONLY;
    else                   attr &= (uint8_t)~FAT32_ATTR_READ_ONLY;
    g_sector_buf[entry_off + 11] = attr;
    if (fat32_bwrite(fs->bdev, entry_sec, g_sector_buf, 1) != 0) return -1;
    return 0;
}

void FAT32_FatstampToDs(uint16_t fat_date, uint16_t fat_time,
                        int32_t *days, int32_t *mins, int32_t *ticks)
{
    fat32_fatstamp_to_ds(fat_date, fat_time, days, mins, ticks);
}

uint32_t FAT32_FatstampToUnix(uint16_t fat_date, uint16_t fat_time)
{
    const int y = 1980 + (fat_date >> 9);
    const unsigned m = (fat_date >> 5) & 0xF;
    const unsigned d = fat_date & 0x1F;
    if (!fat_date || !m || !d) return 0;
    const int32_t unix_days = fat32_days_from_civil(y, m, d);
    if (unix_days < 0) return 0;
    const unsigned h = fat_time >> 11;
    const unsigned mi = (fat_time >> 5) & 0x3F;
    const unsigned s = (fat_time & 0x1F) * 2;
    return (uint32_t)unix_days * 86400 + h * 3600 + mi * 60 + s;
}

/* =========================================================================
 * Volume statistics
 * ========================================================================= */

void FAT32_GetVolumeStats(Fat32FS *fs, uint32_t *total_bytes, uint32_t *used_bytes)
{
    if (!fs) return;
    uint32_t total = fs->total_clusters * fs->cluster_size;

    /* The free count is cached in fs->free_clusters and maintained by
     * fat32_alloc_cluster/fat32_free_chain.  Compute it with a full FAT
     * scan only the first time — on volumes with small clusters the FAT
     * spans tens of thousands of sectors, and rescanning on every
     * `dir`/`info` call makes the shell appear to hang. */
    if (fs->free_clusters < 0) {
        uint32_t free_cnt = 0;
        uint32_t fat_sz = fs->bpb.fat_sz32;
        uint32_t ents_per_sec = fs->bytes_per_sec / 4;
        int scan_ok = 1;

        /* Read the FAT in large chunks — one sector per request turned a
         * full scan into thousands of synchronous device reads (slow, and
         * each one is another chance to hit a device timeout). */
        for (uint32_t sec = 0; sec < fat_sz && scan_ok;
             sec += FAT32_MAX_CLUSTER_SECS) {
            uint32_t batch = fat_sz - sec;
            if (batch > FAT32_MAX_CLUSTER_SECS) batch = FAT32_MAX_CLUSTER_SECS;
            if (BlockDev_Read(fs->bdev, fs->fat_start + sec,
                              g_cluster_buf, batch) != 0 &&
                BlockDev_Read(fs->bdev, fs->fat_start + sec,
                              g_cluster_buf, batch) != 0) {
                scan_ok = 0;   /* retry once, then give up */
                break;
            }
            for (uint32_t i = 0; i < batch * ents_per_sec; i++) {
                uint32_t cluster = sec * ents_per_sec + i;
                if (cluster < 2 || cluster >= fs->total_clusters + 2)
                    continue;
                uint32_t val = le32(&g_cluster_buf[i * 4]) & 0x0FFFFFFF;
                if (val == 0) free_cnt++;
            }
        }

        /* Only cache a complete scan — caching a partial count after an
         * I/O error makes used/free wildly wrong and stays wrong. */
        if (scan_ok) fs->free_clusters = (int32_t)free_cnt;
    }

    uint32_t used = 0;
    if (fs->free_clusters <= (int32_t)fs->total_clusters)
        used = (fs->total_clusters - (uint32_t)fs->free_clusters)
               * fs->cluster_size;

    if (total_bytes) *total_bytes = total;
    if (used_bytes)  *used_bytes = used;
}

int FAT32_VolumeLabel(Fat32FS *fs, char *dst, int max)
{
    if (!fs || !dst || max < 2) return 0;
    /* BPB vol_label is 11 bytes, space-padded, no NUL.  Copy then trim
     * trailing spaces.  "NO NAME" (the formatter's blank label) counts
     * as no label so the unit name remains the display name. */
    int len = 11;
    while (len > 0 && fs->bpb.vol_label[len - 1] == ' ') len--;
    if (len == 0) { dst[0] = '\0'; return 0; }
    if (len == 7 && memcmp(fs->bpb.vol_label, "NO NAME", 7) == 0) {
        dst[0] = '\0';
        return 0;
    }
    if (len > max - 1) len = max - 1;
    int i;
    for (i = 0; i < len; i++) dst[i] = (char)fs->bpb.vol_label[i];
    dst[i] = '\0';
    return len;
}

/* =========================================================================
 * Format
 * ========================================================================= */

/* FAT32_Format writes a fresh FAT32 filesystem to bdev.
 * Returns 0 on success, or a negative error code on failure:
 *   -1  invalid block device
 *   -2  zero capacity device
 *   -3  failed to zero reserved area
 *   -4  failed to write boot sector
 *   -5  failed to write FSINFO
 *   -6  failed to zero FAT
 *   -7  failed to write initial FAT entries
 *   -8  failed to zero root directory
 *   -9  failed to write backup boot sector
 */
int FAT32_Format(BlockDev *bdev, const char *vol_label)
{
    if (!bdev) {
        printf("[FAT32] Invalid block device\n");
        return -1;
    }

    uint64_t total_sectors = BlockDev_GetCapacity(bdev);
    if (total_sectors == 0) {
        printf("[FAT32] Zero capacity device\n");
        return -2;
    }

    /* Compute FAT32 geometry */
    uint32_t bytes_per_sec = 512;
    uint8_t  sec_per_clus  = (total_sectors > 33554432ULL) ? 8 :
                              (total_sectors > 16777216ULL) ? 4 :
                              (total_sectors >  4194304ULL) ? 2 : 1;
    uint16_t rsvd_sec_cnt  = 32;
    uint8_t  num_fats      = 2;
    uint32_t root_clus     = 2;

    /* Iterate to convergence: FAT size depends on cluster count which
     * itself depends on FAT size.  Two passes are NOT always enough —
     * a fixed pair of passes can leave the FAT a few sectors short so
     * the tail clusters have no FAT entries (they then look "used"). */
    uint32_t fat_sz = 1;
    uint32_t data_sectors = 0, total_clusters = 0;
    for (int iter = 0; iter < 16; iter++) {
        data_sectors = (uint32_t)total_sectors - rsvd_sec_cnt
                       - (num_fats * fat_sz);
        total_clusters = data_sectors / sec_per_clus;
        /* entries needed = data clusters + 2 reserved FAT entries */
        uint32_t need = (total_clusters + 2 + 127) / 128;
        if (need <= fat_sz) break;   /* covers all clusters; done */
        fat_sz = need;
    }

    printf("[FAT32] Format: %u sectors, clus=%u, FAT=%u sectors\n",
           (uint32_t)total_sectors, sec_per_clus, fat_sz);

    /* Use the global 4K-aligned g_cluster_buf for all zeroing.
     * This keeps DMA buffers in a known, large, aligned region. */
    memset(g_cluster_buf, 0, sizeof(g_cluster_buf));

    /* --- 1. Zero out boot sector area (0..rsvd_sec_cnt-1) --- */
    if (fat32_bwrite(bdev, 0, g_cluster_buf, rsvd_sec_cnt) != 0) {
        printf("[FAT32] Failed to zero reserved area\n");
        return -3;
    }

    /* --- 2. Build Boot Sector (BPB) in 4K-aligned g_sector_buf --- */
    Fat32BPB *bpb = (Fat32BPB *)g_sector_buf;
    memset(g_sector_buf, 0, 512);

    bpb->jmp_boot[0] = 0xEB;
    bpb->jmp_boot[1] = 0x58;
    bpb->jmp_boot[2] = 0x90;
    memcpy(bpb->oem_name, "UAOS    ", 8);
    bpb->bytes_per_sec = bytes_per_sec;
    bpb->sec_per_clus  = sec_per_clus;
    bpb->rsvd_sec_cnt  = rsvd_sec_cnt;
    bpb->num_fats      = num_fats;
    bpb->root_ent_cnt  = 0;
    bpb->tot_sec16     = 0;
    bpb->media         = 0xF8;
    bpb->fat_sz16      = 0;
    bpb->sec_per_trk   = 63;
    bpb->num_heads     = 255;
    bpb->hidd_sec      = (uint32_t)bdev->part_offset;  /* partition start LBA */
    bpb->tot_sec32     = (uint32_t)total_sectors;
    bpb->fat_sz32      = fat_sz;
    bpb->ext_flags     = 0;
    bpb->fs_ver        = 0;
    bpb->root_clus     = root_clus;
    bpb->fs_info       = 1;  /* FSINFO at sector 1 */
    bpb->bk_boot_sec   = 6;
    bpb->drv_num       = 0x80;
    bpb->reserved1     = 0;
    bpb->boot_sig      = 0x29;
    bpb->vol_id        = 0x12345678;
    if (vol_label && vol_label[0]) {
        /* Pad/truncate to 11 chars, uppercase, space-padded */
        int vi = 0;
        for (vi = 0; vi < 11; vi++) {
            char c = vol_label[vi];
            if (c >= 'a' && c <= 'z') c -= 32;
            bpb->vol_label[vi] = (c != '\0') ? c : ' ';
        }
    } else {
        memcpy(bpb->vol_label, "NO NAME    ", 11);
    }
    memcpy(bpb->fs_type, "FAT32   ", 8);
    bpb->boot_sig55aa  = 0xAA55;

    if (fat32_bwrite(bdev, 0, g_sector_buf, 1) != 0) {
        printf("[FAT32] Failed to write boot sector\n");
        return -4;
    }

    /* Save the BPB in g_sector_buf2 so we can reuse g_sector_buf for the
     * remaining 1-sector writes (FSINFO, initial FAT) while still being able
     * to write the backup boot sector at the end. */
    memcpy(g_sector_buf2, g_sector_buf, 512);

    /* --- 3. FSINFO Sector (sector 1) ---
     * Use g_sector_buf for the FSINFO write.  The mfence fix in the VirtIO
     * driver should resolve the previous timeout on 1-sector writes.
     */
    memset(g_sector_buf, 0, 512);
    *(uint32_t *)(g_sector_buf + 0)   = 0x41615252;  /* lead sig */
    *(uint32_t *)(g_sector_buf + 484) = 0x61417272;  /* struc sig */
    *(uint32_t *)(g_sector_buf + 488) = 0xFFFFFFFF;  /* free count (unknown) */
    *(uint32_t *)(g_sector_buf + 492) = 0xFFFFFFFF;  /* next free (unknown) */
    *(uint32_t *)(g_sector_buf + 508) = 0xAA550000;  /* trail sig */
    if (fat32_bwrite(bdev, 1, g_sector_buf, 1) != 0) {
        printf("[FAT32] Warning: FSINFO write failed (non-fatal)\n");
    }

    /* Re-zero g_cluster_buf before it is used for FAT zeroing. */
    memset(g_cluster_buf, 0, sizeof(g_cluster_buf));

    /* --- 4. Zero FATs (batch up to 64 sectors per write) --- */
    for (uint32_t f = 0; f < num_fats; f++) {
        uint32_t fat_start = rsvd_sec_cnt + (f * fat_sz);
        uint32_t remain = fat_sz;
        uint32_t s = 0;
        while (remain > 0) {
            /* Keep each write <= 128 sectors (64 KB) — the VirtIO data
             * buffer and g_cluster_buf are both 64 KB. */
            uint32_t batch = (remain > 128) ? 128 : remain;
            if (fat32_bwrite(bdev, fat_start + s, g_cluster_buf, batch) != 0) {
                printf("[FAT32] Failed to zero FAT%u at sector %u\n", f, s);
                return -6;
            }
            s += batch;
            remain -= batch;
        }
    }

    /* --- 5. Write initial FAT entries --- */
    memset(g_sector_buf, 0, 512);
    /* Entry 0: media byte in low nibble, high bits set */
    *(uint32_t *)(g_sector_buf + 0) = 0x0FFFFF00 | bpb->media;
    /* Entry 1: EOC marker */
    *(uint32_t *)(g_sector_buf + 4) = 0x0FFFFFFF;
    /* Entry 2 (root dir): EOC marker */
    *(uint32_t *)(g_sector_buf + 8) = 0x0FFFFFFF;

    for (uint32_t f = 0; f < num_fats; f++) {
        uint32_t fat_start = rsvd_sec_cnt + (f * fat_sz);
        printf("[FAT32] Writing initial FAT%u at sector %u\n", f, fat_start);
        if (fat32_bwrite(bdev, fat_start, g_sector_buf, 1) != 0) {
            printf("[FAT32] Failed to write initial FAT%u at sector %u (rc=%d)\n",
                   f, fat_start, 0);
            return -7;
        }
    }

    /* --- 6. Zero root directory cluster --- */
    uint32_t data_start = rsvd_sec_cnt + (num_fats * fat_sz);
    uint32_t root_sec   = data_start + ((root_clus - 2) * sec_per_clus);
    if (fat32_bwrite(bdev, root_sec, g_cluster_buf, sec_per_clus) != 0) {
        printf("[FAT32] Failed to zero root dir\n");
        return -8;
    }

    /* --- 7. Write backup boot sector (sector 6) ---
     * Restore the BPB we saved earlier and write it to the backup location. */
    memcpy(g_sector_buf, g_sector_buf2, 512);
    if (fat32_bwrite(bdev, 6, g_sector_buf, 1) != 0) {
        printf("[FAT32] Failed to write backup boot sector\n");
        return -9;
    }

    printf("[FAT32] Format complete.\n");
    return 0;
}
