/* ffs.c — UAOS Amiga OFS/FFS filesystem driver (read/write)
 *
 * See ffs.h for the on-disk layout contract.  All multi-byte fields are
 * big-endian; every header/data/ext block carries a longword checksum such
 * that the sum of all 128 longwords is zero.  Chain walks are bounded to
 * keep corrupt media from looping forever.
 *
 * DMA note: all block reads go through two static 4K-aligned buffers —
 * the VirtIO driver requires DMA-accessible, non-stack buffers.
 */

#include "ffs.h"
#include "../boot/kprint.h"
#include "../irq/rtc.h"
#include <string.h>

/* =========================================================================
 * Static state
 * ========================================================================= */

#define FFS_MAX_VOLS 4

static FfsVolume g_ffs_vols[FFS_MAX_VOLS];

/* DMA-safe block buffers.  g_ffs_hdr is used for header/ext-block decoding,
 * g_ffs_data for data-block reads — kept separate so a ReadAt ext-chain
 * walk does not clobber the header block in flight. */
static uint8_t g_ffs_hdr[FFS_BLOCK_BYTES]  __attribute__((aligned(4096)));
static uint8_t g_ffs_data[FFS_BLOCK_BYTES] __attribute__((aligned(4096)));
static uint8_t g_ffs_scan[FFS_BLOCK_BYTES] __attribute__((aligned(4096)));

/* =========================================================================
 * Big-endian + BSTR helpers
 * ========================================================================= */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int32_t be32s(const uint8_t *p) { return (int32_t)be32(p); }

static uint32_t blk_lw(const uint8_t *blk, int i) { return be32(blk + i * 4); }
static int32_t  blk_lws(const uint8_t *blk, int i) { return be32s(blk + i * 4); }

static int blk_checksum_ok(const uint8_t *blk)
{
    uint32_t sum = 0;
    for (int i = 0; i < FFS_BLOCK_LONGS; i++)
        sum += be32(blk + i * 4);
    return sum == 0;
}

/* Copy a BSTR (len byte + chars, max 30) into a C string. */
static void bstr_to_c(const uint8_t *src, char *dst, int max)
{
    int n = src[0];
    if (n > max - 1) n = max - 1;
    int i = 0;
    for (; i < n; i++) dst[i] = (char)src[1 + i];
    dst[i] = '\0';
}

static int c_upper(int c)
{
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

/* Case-insensitive compare of a C string against a BSTR field. */
static int bstr_eq_ci(const uint8_t *bstr, const char *name)
{
    int n = bstr[0];
    int i = 0;
    for (; i < n && name[i]; i++)
        if (c_upper((uint8_t)bstr[1 + i]) != c_upper((uint8_t)name[i]))
            return 0;
    return i == n && name[i] == '\0';
}

uint32_t FFS_Hash(const char *name, uint32_t ht_size)
{
    /* AmigaDOS name hash: seed with the length, fold each char as
     * h = h*13 + c (masked to 11 bits), then reduce mod ht_size. */
    uint32_t h = 0;
    while (name[h]) h++;
    uint32_t len = h;
    h = len;
    for (uint32_t i = 0; i < len; i++)
        h = (h * 13 + (uint32_t)c_upper((uint8_t)name[i])) & 0x7FF;
    return ht_size ? h % ht_size : 0;
}

/* =========================================================================
 * Block I/O helpers
 * ========================================================================= */

static int ffs_read_block(FfsVolume *vol, uint32_t key, uint8_t *dst)
{
    if (key >= vol->num_blocks) return -1;
    return BlockDev_Read(vol->dev, key, dst, 1);
}

/* Read and checksum-validate a header-ish block into g_ffs_hdr.
 * Returns 0 on success. */
static int ffs_read_hdr(FfsVolume *vol, uint32_t key)
{
    if (ffs_read_block(vol, key, g_ffs_hdr) != 0) return -1;
    if (!blk_checksum_ok(g_ffs_hdr)) return -2;
    return 0;
}

/* =========================================================================
 * Probe / mount
 * ========================================================================= */

int FFS_Probe(BlockDev *dev)
{
    if (!dev || dev->sector_size != FFS_BLOCK_BYTES) return 0;
    memset(g_ffs_scan, 0, sizeof(g_ffs_scan));
    if (BlockDev_Read(dev, 0, g_ffs_scan, 1) != 0) return 0;
    return g_ffs_scan[0] == 'D' && g_ffs_scan[1] == 'O' &&
           g_ffs_scan[2] == 'S' && g_ffs_scan[3] <= 7;
}

/* Validate a candidate root block already loaded in `blk`. */
static int ffs_root_ok(const uint8_t *blk, uint32_t num_blocks)
{
    if (blk_lw(blk, FFS_LW_TYPE) != FFS_T_HEADER) return 0;
    if (blk_lws(blk, FFS_LW_SEC_TYPE) != FFS_ST_ROOT) return 0;
    if (!blk_checksum_ok(blk)) return 0;
    uint32_t ht = blk_lw(blk, FFS_LW_HT_SIZE);
    if (ht == 0 || ht > FFS_BLOCK_LONGS - 56) return 0;
    /* Hash table entries must be in range or zero. */
    for (uint32_t i = 0; i < ht; i++) {
        uint32_t k = blk_lw(blk, FFS_LW_TABLE + i);
        if (k >= num_blocks) return 0;
    }
    return 1;
}

FfsVolume *FFS_Mount(BlockDev *dev)
{
    if (!dev || dev->sector_size != FFS_BLOCK_BYTES) return NULL;
    if (!FFS_Probe(dev)) return NULL;

    FfsVolume *vol = NULL;
    for (int i = 0; i < FFS_MAX_VOLS; i++)
        if (!g_ffs_vols[i].in_use) { vol = &g_ffs_vols[i]; break; }
    if (!vol) return NULL;
    memset(vol, 0, sizeof(*vol));
    vol->dev = dev;
    vol->dostype = g_ffs_scan[3] & 7;
    vol->is_ofs = (vol->dostype & 1) == 0;
    vol->is_intl = (vol->dostype & 2) != 0;
    vol->block_size = FFS_BLOCK_BYTES;
    vol->num_blocks = (uint32_t)dev->num_sectors;
    vol->ht_size = FFS_BLOCK_LONGS - 56;   /* 72 for 512-byte blocks */

    /* Root block sits in the middle of the volume; scan a window around
     * num_blocks/2 for a valid root in case of odd geometries. */
    uint32_t mid = vol->num_blocks / 2;
    uint32_t found = 0;
    for (int d = 0; d < 64 && !found; d++) {
        for (int s = 0; s < 2; s++) {
            uint32_t cand = (s == 0) ? mid + (uint32_t)d
                                     : mid - (uint32_t)d;
            if (cand >= vol->num_blocks) continue;
            if (ffs_read_block(vol, cand, g_ffs_scan) != 0) continue;
            if (ffs_root_ok(g_ffs_scan, vol->num_blocks)) {
                found = cand + 1;
                break;
            }
            if (s == 0 && d == 0) break;   /* don't test mid twice */
        }
    }
    if (!found) {
        kprint("[FFS] no valid root block found\n");
        return NULL;
    }
    vol->root_key = found - 1;

    /* Re-validate into the hdr buffer and decode the volume fields. */
    if (ffs_read_block(vol, vol->root_key, g_ffs_scan) != 0) return NULL;

    vol->bitmap_valid = (blk_lws(g_ffs_scan, FFS_LW_BM_FLAG) == -1);
    bstr_to_c(g_ffs_scan + FFS_LW_NAME * 4, vol->vol_name, sizeof(vol->vol_name));
    vol->in_use = 1;

    /* Best-effort used-block count (bitmap is advisory). */
    {
        int32_t used = FFS_CountUsedBlocks(vol);
        vol->used_blocks = (used >= 0) ? (uint32_t)used : 0;
    }

    kprint("[FFS] mounted '");
    kprint(vol->vol_name);
    kprint("' dostype=DOS\\");
    kprinthex(vol->dostype);
    kprint(" root=");
    kprinthex(vol->root_key);
    kprint("\n");
    return vol;
}

void FFS_Unmount(FfsVolume *vol)
{
    if (vol) vol->in_use = 0;
}

const char *FFS_VolumeName(FfsVolume *vol)
{
    return vol ? vol->vol_name : "";
}

uint32_t FFS_DosType(FfsVolume *vol)
{
    if (!vol) return 0;
    return 0x444F5300u | vol->dostype;
}

/* =========================================================================
 * Entry decode
 * ========================================================================= */

int FFS_ReadEntry(FfsVolume *vol, uint32_t key, FfsEntry *out)
{
    if (!vol || !out) return -1;
    if (ffs_read_hdr(vol, key) != 0) return -1;
    if (blk_lw(g_ffs_hdr, FFS_LW_TYPE) != FFS_T_HEADER) return -2;

    memset(out, 0, sizeof(*out));
    out->key = key;
    out->sec_type = blk_lws(g_ffs_hdr, FFS_LW_SEC_TYPE);
    out->byte_size = blk_lw(g_ffs_hdr, FFS_LW_BYTE_SIZE);
    out->protect = blk_lw(g_ffs_hdr, FFS_LW_PROTECT);
    out->parent_key = blk_lw(g_ffs_hdr, FFS_LW_PARENT);
    out->ext_key = blk_lw(g_ffs_hdr, FFS_LW_EXTENSION);
    out->next_hash = blk_lw(g_ffs_hdr, FFS_LW_NEXT_HASH);
    out->high_seq = blk_lw(g_ffs_hdr, FFS_LW_HIGH_SEQ);
    out->days = blk_lw(g_ffs_hdr, FFS_LW_DAYS);
    out->mins = blk_lw(g_ffs_hdr, FFS_LW_MINS);
    out->ticks = blk_lw(g_ffs_hdr, FFS_LW_TICKS);
    bstr_to_c(g_ffs_hdr + FFS_LW_NAME * 4, out->name, sizeof(out->name));
    /* lw82+ overlaps the root block's bitmap page keys — only decode the
     * comment for real dir/file entries. */
    if (out->sec_type != FFS_ST_ROOT)
        bstr_to_c(g_ffs_hdr + FFS_LW_COMMENT * 4, out->comment,
                  sizeof(out->comment));

    switch (out->sec_type) {
        case FFS_ST_ROOT:
        case FFS_ST_USERDIR:
        case FFS_ST_FILE:
        case FFS_ST_LINKDIR:
        case FFS_ST_LINKFILE:
            return 0;
        default:
            return -3;
    }
}

/* =========================================================================
 * Lookup / resolve
 * ========================================================================= */

uint32_t FFS_Lookup(FfsVolume *vol, uint32_t dir_key, const char *name)
{
    if (!vol || !name || !name[0]) return 0;
    if (ffs_read_hdr(vol, dir_key) != 0) return 0;

    uint32_t slot = FFS_Hash(name, vol->ht_size);
    uint32_t key = blk_lw(g_ffs_hdr, FFS_LW_TABLE + slot);
    int guard = vol->num_blocks > 1024 ? (int)vol->num_blocks : 1024;

    while (key && key < vol->num_blocks && guard-- > 0) {
        if (ffs_read_hdr(vol, key) != 0) return 0;
        if (bstr_eq_ci(g_ffs_hdr + FFS_LW_NAME * 4, name))
            return key;
        key = blk_lw(g_ffs_hdr, FFS_LW_NEXT_HASH);
    }
    return 0;
}

uint32_t FFS_Resolve(FfsVolume *vol, const char *path)
{
    if (!vol) return 0;
    uint32_t cur = vol->root_key;
    if (!path) return cur;

    /* Skip the volume prefix up to ':' if one was passed. */
    const char *p = path;
    const char *colon = p;
    while (*colon && *colon != ':') colon++;
    if (*colon == ':') p = colon + 1;

    char comp[FFS_MAX_NAME];
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        int n = 0;
        while (*p && *p != '/' && n < FFS_MAX_NAME - 1)
            comp[n++] = *p++;
        comp[n] = '\0';
        if (!n) break;

        if (n == 1 && comp[0] == '.') continue;
        if (n == 2 && comp[0] == '.' && comp[1] == '.') {
            FfsEntry e;
            if (FFS_ReadEntry(vol, cur, &e) == 0 && e.parent_key)
                cur = e.parent_key;
            continue;
        }

        cur = FFS_Lookup(vol, cur, comp);
        if (!cur) return 0;
    }
    return cur;
}

/* =========================================================================
 * Directory enumeration
 * ========================================================================= */

void FFS_DirIterInit(FfsDirIter *it)
{
    if (it) { it->ht_idx = 0; it->chain_key = 0; }
}

int FFS_DirIterNext(FfsVolume *vol, uint32_t dir_key, FfsDirIter *it,
                    FfsEntry *out)
{
    if (!vol || !it || !out) return 0;

    uint32_t guard = vol->num_blocks > 4096 ? vol->num_blocks : 4096;
    while (guard-- > 0) {
        /* Continue an in-progress same-hash chain first. */
        if (it->chain_key) {
            uint32_t key = it->chain_key;
            it->chain_key = 0;
            if (key < vol->num_blocks &&
                FFS_ReadEntry(vol, key, out) == 0) {
                it->chain_key = out->next_hash;
                return 1;
            }
            continue;
        }
        if (it->ht_idx >= vol->ht_size) return 0;

        /* Re-read the directory block: the earlier entry decode may have
         * overwritten g_ffs_hdr. */
        if (ffs_read_hdr(vol, dir_key) != 0) return 0;
        uint32_t key = blk_lw(g_ffs_hdr, FFS_LW_TABLE + it->ht_idx);
        it->ht_idx++;
        if (key == 0) continue;
        if (key >= vol->num_blocks) return 0;
        if (FFS_ReadEntry(vol, key, out) != 0) return 0;
        it->chain_key = out->next_hash;
        return 1;
    }
    return 0;
}

/* =========================================================================
 * File read
 * ========================================================================= */

/* Data-block pointer `seq` (0-based) of file `hdr_key`; walks T.LIST
 * extension blocks when the header's own table is exhausted.  Both file
 * headers and extension blocks anchor the data-pointer table at the top
 * of the 72-entry region: ptr i lives at longword index 5 + ht_size - i. */
static uint32_t ffs_data_key(FfsVolume *vol, uint32_t hdr_key, uint32_t seq)
{
    if (ffs_read_hdr(vol, hdr_key) != 0) return 0;
    uint32_t high = blk_lw(g_ffs_hdr, FFS_LW_HIGH_SEQ);
    if (high > vol->ht_size) high = vol->ht_size;
    if (seq < high)
        return blk_lw(g_ffs_hdr, 5 + (int32_t)vol->ht_size - (int32_t)seq);
    seq -= high;

    uint32_t ext = blk_lw(g_ffs_hdr, FFS_LW_EXTENSION);
    int guard = 256;
    while (ext && ext < vol->num_blocks && guard-- > 0) {
        if (ffs_read_hdr(vol, ext) != 0) return 0;
        if (blk_lw(g_ffs_hdr, FFS_LW_TYPE) != FFS_T_LIST) return 0;
        high = blk_lw(g_ffs_hdr, FFS_LW_HIGH_SEQ);
        if (high > vol->ht_size) high = vol->ht_size;
        if (seq < high)
            return blk_lw(g_ffs_hdr, 5 + (int32_t)vol->ht_size - (int32_t)seq);
        seq -= high;
        ext = blk_lw(g_ffs_hdr, FFS_LW_EXTENSION);
    }
    return 0;
}

int32_t FFS_ReadAt(FfsVolume *vol, const FfsEntry *entry,
                   uint32_t pos, void *dst, uint32_t len)
{
    if (!vol || !entry || !dst) return -1;
    if (entry->sec_type != FFS_ST_FILE && entry->sec_type != FFS_ST_LINKFILE)
        return -2;
    if (pos >= entry->byte_size) return 0;
    if (pos + len > entry->byte_size) len = entry->byte_size - pos;

    const uint32_t payload = vol->is_ofs ? FFS_OFS_PAYLOAD : vol->block_size;
    uint8_t *out = (uint8_t *)dst;
    uint32_t done = 0;

    while (done < len) {
        uint32_t seq = (pos + done) / payload;
        uint32_t off = (pos + done) % payload;
        uint32_t want = payload - off;
        if (want > len - done) want = len - done;

        uint32_t dkey = ffs_data_key(vol, entry->key, seq);
        if (!dkey || dkey >= vol->num_blocks) break;
        if (ffs_read_block(vol, dkey, g_ffs_data) != 0) break;

        if (vol->is_ofs) {
            /* OFS data block: 24-byte header, declared payload size. */
            if (blk_lw(g_ffs_data, 0) != FFS_T_DATA ||
                !blk_checksum_ok(g_ffs_data))
                break;
            uint32_t dsz = blk_lw(g_ffs_data, 3);
            if (off >= dsz) break;
            if (want > dsz - off) want = dsz - off;
            memcpy(out + done, g_ffs_data + FFS_OFS_DATA_HDR + off, want);
        } else {
            memcpy(out + done, g_ffs_data + off, want);
        }
        done += want;
    }
    return (int32_t)done;
}

uint32_t FFS_FileSize(FfsVolume *vol, uint32_t key)
{
    FfsEntry e;
    if (FFS_ReadEntry(vol, key, &e) != 0) return 0;
    if (e.sec_type != FFS_ST_FILE && e.sec_type != FFS_ST_LINKFILE) return 0;
    return e.byte_size;
}

/* =========================================================================
 * Bitmap (advisory — used by fsck and DISK_INFO)
 * ========================================================================= */

static int ffs_bitmap_bit(FfsVolume *vol, uint32_t block, int *is_free)
{
    /* Collect bitmap page keys lazily on each call; volumes are small
     * enough that walking the 25 root pages plus the bm_ext index chain
     * per query is acceptable for diagnostics. */
    if (!vol->bitmap_valid) return 0;
    const uint32_t bits_per_page = 127 * 32;
    uint32_t page = block / bits_per_page;
    uint32_t off = block % bits_per_page;

    /* Read the root block for the first 25 page keys. */
    if (ffs_read_block(vol, vol->root_key, g_ffs_scan) != 0) return 0;
    uint32_t page_key;
    if (page < 25) {
        page_key = blk_lw(g_ffs_scan, FFS_LW_BM_PAGES + page);
    } else {
        /* Walk the bm_ext index chain: each index block holds up to
         * 127 bitmap page keys plus a next-index link at lw127. */
        uint32_t ext = blk_lw(g_ffs_scan, FFS_LW_BM_EXT);
        uint32_t idx = 25;
        int guard = 64;
        page_key = 0;
        while (ext && ext < vol->num_blocks && guard-- > 0) {
            if (ffs_read_block(vol, ext, g_ffs_scan) != 0) return 0;
            for (int i = 0; i < 127; i++) {
                uint32_t k = blk_lw(g_ffs_scan, i);
                if (!k) break;
                if (idx == page) { page_key = k; break; }
                idx++;
            }
            if (page_key) break;
            ext = blk_lw(g_ffs_scan, 127);
        }
    }
    if (!page_key || page_key >= vol->num_blocks) return 0;
    if (ffs_read_block(vol, page_key, g_ffs_scan) != 0) return 0;
    uint32_t w = blk_lw(g_ffs_scan, 1 + off / 32);
    *is_free = (int)((w >> (off % 32)) & 1);
    return 1;
}

int FFS_BlockIsFree(FfsVolume *vol, uint32_t block, int *is_free)
{
    if (!vol || !is_free || block >= vol->num_blocks) return 0;
    return ffs_bitmap_bit(vol, block, is_free);
}

int32_t FFS_CountUsedBlocks(FfsVolume *vol)
{
    if (!vol || !vol->bitmap_valid) return -1;
    if (ffs_read_block(vol, vol->root_key, g_ffs_scan) != 0) return -1;

    /* Gather page keys: 25 in the root block, remainder via bm_ext. */
    uint32_t keys[96];
    int n = 0;
    for (int i = 0; i < 25; i++) {
        uint32_t k = blk_lw(g_ffs_scan, FFS_LW_BM_PAGES + i);
        if (k && k < vol->num_blocks) keys[n++] = k;
    }
    uint32_t ext = blk_lw(g_ffs_scan, FFS_LW_BM_EXT);
    int guard = 16;
    while (ext && ext < vol->num_blocks && guard-- > 0 &&
           n < (int)(sizeof(keys) / sizeof(keys[0]))) {
        if (ffs_read_block(vol, ext, g_ffs_scan) != 0) break;
        for (int i = 0; i < 127; i++) {
            uint32_t k = blk_lw(g_ffs_scan, i);
            if (!k) break;
            if (k < vol->num_blocks &&
                n < (int)(sizeof(keys) / sizeof(keys[0])))
                keys[n++] = k;
        }
        ext = blk_lw(g_ffs_scan, 127);
    }

    int32_t used = 0;
    uint32_t counted = 0;
    for (int p = 0; p < n && counted < vol->num_blocks; p++) {
        if (ffs_read_block(vol, keys[p], g_ffs_scan) != 0) continue;
        for (int i = 1; i < 128 && counted < vol->num_blocks; i++) {
            uint32_t w = blk_lw(g_ffs_scan, i);
            uint32_t rem = vol->num_blocks - counted;
            /* bit set = free; count zeros, clamped at num_blocks */
            if (rem < 32) w &= (1u << rem) - 1;
            uint32_t free_bits = 0;
            while (w) { free_bits += w & 1u; w >>= 1; }
            used += (int32_t)((rem < 32 ? rem : 32u) - free_bits);
            counted += 32;
        }
    }
    return used;
}

/* =========================================================================
 * Write support
 * ========================================================================= */

static void blk_put32(uint8_t *blk, int i, uint32_t v)
{
    blk[i * 4 + 0] = (uint8_t)(v >> 24);
    blk[i * 4 + 1] = (uint8_t)(v >> 16);
    blk[i * 4 + 2] = (uint8_t)(v >> 8);
    blk[i * 4 + 3] = (uint8_t)v;
}

/* Header/ext/data-block checksum (lw5): sum of all longwords == 0. */
static void blk_fix_checksum(uint8_t *blk)
{
    blk_put32(blk, FFS_LW_CHECKSUM, 0);
    uint32_t sum = 0;
    for (int i = 0; i < FFS_BLOCK_LONGS; i++)
        sum += be32(blk + i * 4);
    blk_put32(blk, FFS_LW_CHECKSUM, (uint32_t)(-(int32_t)sum));
}

/* Bitmap-block checksum (lw0): same sum rule, different slot. */
static void blk_fix_bm_checksum(uint8_t *blk)
{
    blk_put32(blk, 0, 0);
    uint32_t sum = 0;
    for (int i = 0; i < FFS_BLOCK_LONGS; i++)
        sum += be32(blk + i * 4);
    blk_put32(blk, 0, (uint32_t)(-(int32_t)sum));
}

/* Write g_ffs_hdr back to `key` with a fresh checksum. */
static int ffs_write_hdr(FfsVolume *vol, uint32_t key)
{
    blk_fix_checksum(g_ffs_hdr);
    return BlockDev_Write(vol->dev, key, g_ffs_hdr, 1);
}

/* AmigaDOS DateStamp for "now" (days since 1978, mins, 50Hz ticks). */
static void ffs_now(uint32_t *days, uint32_t *mins, uint32_t *ticks)
{
    RtcDateTime dt = RTC_ReadDateTime();
    static const int mdays[12] =
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    uint32_t d = 0;
    for (int y = 1978; y < (int)dt.year; y++) {
        int leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
        d += leap ? 366 : 365;
    }
    int leap = ((dt.year % 4 == 0) && (dt.year % 100 != 0)) ||
               (dt.year % 400 == 0);
    for (int m = 1; m < (int)dt.month && m <= 12; m++) {
        d += (uint32_t)mdays[m - 1];
        if (m == 2 && leap) d++;
    }
    if (dt.day > 0) d += dt.day - 1;
    *days = d;
    *mins = (uint32_t)dt.hour * 60 + dt.min;
    *ticks = (uint32_t)dt.sec * 50;
}

/* C string -> BSTR field (len byte + chars, zero-padded area). */
static void bstr_from_c(uint8_t *dst, const char *src, int max)
{
    int n = 0;
    while (src && src[n] && n < max) {
        dst[1 + n] = (uint8_t)src[n];
        n++;
    }
    dst[0] = (uint8_t)n;
}

/* ------------------------------------------------------------------
 * Bitmap allocation
 * ------------------------------------------------------------------ */

/* Gather all bitmap page keys (25 root entries + bm_ext chain). */
static int ffs_bitmap_pages(FfsVolume *vol, uint32_t *keys, int max)
{
    if (!vol->bitmap_valid) return 0;
    if (ffs_read_block(vol, vol->root_key, g_ffs_scan) != 0) return 0;
    int n = 0;
    for (int i = 0; i < 25 && n < max; i++) {
        uint32_t k = blk_lw(g_ffs_scan, FFS_LW_BM_PAGES + i);
        if (k && k < vol->num_blocks) keys[n++] = k;
    }
    uint32_t ext = blk_lw(g_ffs_scan, FFS_LW_BM_EXT);
    int guard = 16;
    while (ext && ext < vol->num_blocks && guard-- > 0 && n < max) {
        if (ffs_read_block(vol, ext, g_ffs_scan) != 0) break;
        for (int i = 0; i < 127; i++) {
            uint32_t k = blk_lw(g_ffs_scan, i);
            if (!k) break;
            if (k < vol->num_blocks && n < max) keys[n++] = k;
        }
        ext = blk_lw(g_ffs_scan, 127);
    }
    return n;
}

static int ffs_bitmap_set(FfsVolume *vol, uint32_t block, int free_it)
{
    if (block >= vol->num_blocks) return -1;
    const uint32_t bits_per_page = 127 * 32;
    uint32_t page = block / bits_per_page;
    uint32_t off  = block % bits_per_page;
    uint32_t keys[96];
    int n = ffs_bitmap_pages(vol, keys, 96);
    if (page >= (uint32_t)n) return -1;

    if (ffs_read_block(vol, keys[page], g_ffs_scan) != 0) return -1;
    uint32_t wi = 1 + off / 32;
    uint32_t bit = 1u << (off % 32);
    uint32_t w = blk_lw(g_ffs_scan, (int)wi);
    if (free_it) w |= bit; else w &= ~bit;
    blk_put32(g_ffs_scan, (int)wi, w);
    blk_fix_bm_checksum(g_ffs_scan);
    if (BlockDev_Write(vol->dev, keys[page], g_ffs_scan, 1) != 0)
        return -1;
    if (free_it) {
        if (vol->used_blocks) vol->used_blocks--;
    } else {
        vol->used_blocks++;
    }
    return 0;
}

uint32_t FFS_AllocBlock(FfsVolume *vol)
{
    if (!vol || !vol->bitmap_valid) {
        kprint("[FFS] alloc: bitmap not valid\n");
        return 0;
    }
    uint32_t keys[96];
    int n = ffs_bitmap_pages(vol, keys, 96);
    uint32_t base = 0;
    for (int p = 0; p < n; p++, base += 127 * 32) {
        if (ffs_read_block(vol, keys[p], g_ffs_scan) != 0) {
            kprint("[FFS] alloc: page read fail\n");
            continue;
        }
        for (int w = 1; w < 128; w++) {
            uint32_t word = blk_lw(g_ffs_scan, w);
            if (!word) continue;   /* no free bits */
            for (int b = 0; b < 32; b++) {
                uint32_t blkno = base + (uint32_t)(w - 1) * 32 + (uint32_t)b;
                if (blkno >= vol->num_blocks) break;
                if (!(word & (1u << b))) continue;
                word &= ~(1u << b);
                blk_put32(g_ffs_scan, w, word);
                /* Blocks 0-1 are the bootblock area and are never
                 * allocatable.  The bitmap is advisory and may leave
                 * them marked free (WinUAE does) — claim them here so
                 * they are neither returned nor double-allocated. */
                if (blkno < 2) {
                    blk_fix_bm_checksum(g_ffs_scan);
                    BlockDev_Write(vol->dev, keys[p], g_ffs_scan, 1);
                    vol->used_blocks++;
                    continue;
                }
                blk_fix_bm_checksum(g_ffs_scan);
                if (BlockDev_Write(vol->dev, keys[p], g_ffs_scan, 1) != 0)
                    return 0;
                vol->used_blocks++;
                return blkno;
            }
        }
    }
    return 0;
}

void FFS_FreeBlock(FfsVolume *vol, uint32_t block)
{
    if (!vol || !vol->bitmap_valid) return;
    ffs_bitmap_set(vol, block, 1);
}

/* ------------------------------------------------------------------
 * File data-pointer table management
 * ------------------------------------------------------------------ */

/* Total data pointers across header + T.LIST chain. */
static uint32_t ffs_data_count(FfsVolume *vol, uint32_t hdr_key)
{
    uint32_t total = 0, key = hdr_key;
    int guard = 512;
    while (key && key < vol->num_blocks && guard-- > 0) {
        if (ffs_read_hdr(vol, key) != 0) break;
        uint32_t high = blk_lw(g_ffs_hdr, FFS_LW_HIGH_SEQ);
        if (high > vol->ht_size) high = vol->ht_size;
        total += high;
        key = blk_lw(g_ffs_hdr, FFS_LW_EXTENSION);
    }
    return total;
}

/* Store `dkey` at position `high` inside the pointer table of the block
 * currently in g_ffs_hdr (holder `key`), then write it back. */
static int ffs_store_ptr(FfsVolume *vol, uint32_t key, uint32_t high,
                         uint32_t dkey)
{
    blk_put32(g_ffs_hdr, FFS_LW_TABLE + (int)vol->ht_size - 1 - (int)high,
              dkey);
    blk_put32(g_ffs_hdr, FFS_LW_HIGH_SEQ, high + 1);
    return ffs_write_hdr(vol, key);
}

/* Append `dkey` to the file's pointer tables, allocating a T.LIST
 * extension block when every existing table is full. */
static int ffs_append_ptr(FfsVolume *vol, uint32_t hdr_key, uint32_t dkey)
{
    if (ffs_read_hdr(vol, hdr_key) != 0) return -1;
    uint32_t holder = hdr_key;
    uint32_t high = blk_lw(g_ffs_hdr, FFS_LW_HIGH_SEQ);
    if (high > vol->ht_size) return -1;

    if (high == vol->ht_size) {
        /* Header full — find a T.LIST block with room, or make one. */
        uint32_t ext = blk_lw(g_ffs_hdr, FFS_LW_EXTENSION);
        int have_room = 0, guard = 256;
        while (ext && ext < vol->num_blocks && guard-- > 0) {
            if (ffs_read_hdr(vol, ext) != 0) return -1;
            if (blk_lw(g_ffs_hdr, FFS_LW_TYPE) != FFS_T_LIST) return -1;
            holder = ext;
            high = blk_lw(g_ffs_hdr, FFS_LW_HIGH_SEQ);
            if (high > vol->ht_size) return -1;
            if (high < vol->ht_size) { have_room = 1; break; }
            ext = blk_lw(g_ffs_hdr, FFS_LW_EXTENSION);
        }
        if (!have_room) {
            /* `holder` (hdr or last ext) is the chain end — link a new
             * T.LIST block onto it. */
            uint32_t newext = FFS_AllocBlock(vol);
            if (!newext) return -2;
            if (ffs_read_hdr(vol, holder) != 0) {
                FFS_FreeBlock(vol, newext);
                return -1;
            }
            blk_put32(g_ffs_hdr, FFS_LW_EXTENSION, newext);
            if (ffs_write_hdr(vol, holder) != 0) {
                blk_put32(g_ffs_hdr, FFS_LW_EXTENSION, 0);
                blk_fix_checksum(g_ffs_hdr);
                FFS_FreeBlock(vol, newext);
                return -1;
            }
            memset(g_ffs_hdr, 0, FFS_BLOCK_BYTES);
            blk_put32(g_ffs_hdr, FFS_LW_TYPE, FFS_T_LIST);
            blk_put32(g_ffs_hdr, FFS_LW_HEADER_KEY, newext);
            blk_put32(g_ffs_hdr, FFS_LW_PARENT, hdr_key);
            blk_put32(g_ffs_hdr, FFS_LW_SEC_TYPE, (uint32_t)FFS_ST_FILE);
            holder = newext;
            high = 0;
            /* g_ffs_hdr holds the fresh ext block — store into it. */
            return ffs_store_ptr(vol, holder, high, dkey);
        }
    }
    /* g_ffs_hdr holds `holder` with room for one more pointer. */
    return ffs_store_ptr(vol, holder, high, dkey);
}

/* Allocate + initialise a new data block and append it to `hdr_key`'s
 * tables at sequence position `seq` (0-based).  For OFS the block gets a
 * 24-byte header and the previous data block's nextData link is set. */
static uint32_t ffs_file_append_block(FfsVolume *vol, uint32_t hdr_key,
                                      uint32_t seq)
{
    uint32_t dkey = FFS_AllocBlock(vol);
    if (!dkey) return 0;

    memset(g_ffs_data, 0, FFS_BLOCK_BYTES);
    if (vol->is_ofs) {
        blk_put32(g_ffs_data, FFS_OFS_LW_TYPE, FFS_T_DATA);
        blk_put32(g_ffs_data, FFS_OFS_LW_HDR_KEY, hdr_key);
        blk_put32(g_ffs_data, FFS_OFS_LW_SEQ, seq + 1);   /* 1-based */
        blk_fix_checksum(g_ffs_data);   /* lw5 slot — same rule */
    }
    if (BlockDev_Write(vol->dev, dkey, g_ffs_data, 1) != 0) {
        FFS_FreeBlock(vol, dkey);
        return 0;
    }
    if (ffs_append_ptr(vol, hdr_key, dkey) != 0) {
        FFS_FreeBlock(vol, dkey);
        return 0;
    }
    /* Chain the previous OFS data block forward. */
    if (vol->is_ofs && seq > 0) {
        uint32_t prev = ffs_data_key(vol, hdr_key, seq - 1);
        if (prev && prev < vol->num_blocks &&
            ffs_read_block(vol, prev, g_ffs_data) == 0) {
            blk_put32(g_ffs_data, FFS_OFS_LW_NEXT, dkey);
            blk_fix_checksum(g_ffs_data);
            BlockDev_Write(vol->dev, prev, g_ffs_data, 1);
        }
    }
    return dkey;
}

/* Free every T.LIST extension block starting at `first_ext`. */
static void ffs_free_ext_chain(FfsVolume *vol, uint32_t first_ext)
{
    int guard = 512;
    while (first_ext && first_ext < vol->num_blocks && guard-- > 0) {
        if (ffs_read_block(vol, first_ext, g_ffs_hdr) != 0) return;
        uint32_t nxt = blk_lw(g_ffs_hdr, FFS_LW_EXTENSION);
        FFS_FreeBlock(vol, first_ext);
        first_ext = nxt;
    }
}

/* Trim pointer tables to `keep` entries: zero dropped slots, drop the
 * high_seq counts, and free+unlink ext blocks entirely beyond `keep`.
 * Data blocks must already have been freed by the caller. */
static void ffs_trim_ptrs(FfsVolume *vol, uint32_t hdr_key, uint32_t keep)
{
    uint32_t node = hdr_key, base = 0;
    int guard = 512;
    while (node && guard-- > 0) {
        if (ffs_read_hdr(vol, node) != 0) return;
        uint32_t high = blk_lw(g_ffs_hdr, FFS_LW_HIGH_SEQ);
        if (high > vol->ht_size) high = vol->ht_size;
        uint32_t ext = blk_lw(g_ffs_hdr, FFS_LW_EXTENSION);
        if (base + high > keep) {
            uint32_t trim = (keep > base) ? keep - base : 0;
            for (uint32_t i = trim; i < high; i++)
                blk_put32(g_ffs_hdr,
                          FFS_LW_TABLE + (int)vol->ht_size - 1 - (int)i, 0);
            blk_put32(g_ffs_hdr, FFS_LW_HIGH_SEQ, trim);
            if (ext) {
                blk_put32(g_ffs_hdr, FFS_LW_EXTENSION, 0);
                ffs_write_hdr(vol, node);
                ffs_free_ext_chain(vol, ext);
            } else {
                ffs_write_hdr(vol, node);
            }
            return;
        }
        base += high;
        node = ext;
    }
}

/* Set the last data block's declared payload + end-of-chain marker for
 * OFS volumes after a size change. */
static void ffs_ofs_fix_tail(FfsVolume *vol, uint32_t hdr_key,
                             uint32_t need, uint32_t new_size)
{
    if (!need) return;
    const uint32_t payload = FFS_OFS_PAYLOAD;
    uint32_t last = ffs_data_key(vol, hdr_key, need - 1);
    if (!last || last >= vol->num_blocks) return;
    if (ffs_read_block(vol, last, g_ffs_data) != 0) return;
    uint32_t rem = new_size - (need - 1) * payload;
    if (rem > payload) rem = payload;
    blk_put32(g_ffs_data, FFS_OFS_LW_SIZE, rem);
    blk_put32(g_ffs_data, FFS_OFS_LW_NEXT, 0);
    blk_fix_checksum(g_ffs_data);
    BlockDev_Write(vol->dev, last, g_ffs_data, 1);
}

int FFS_SetFileSize(FfsVolume *vol, uint32_t key, uint32_t new_size)
{
    FfsEntry e;
    if (FFS_ReadEntry(vol, key, &e) != 0) return -1;
    if (e.sec_type != FFS_ST_FILE && e.sec_type != FFS_ST_LINKFILE)
        return -2;

    const uint32_t payload = vol->is_ofs ? FFS_OFS_PAYLOAD : vol->block_size;
    uint32_t need = (new_size + payload - 1) / payload;
    uint32_t count = ffs_data_count(vol, key);

    if (count < need) {
        /* Extend with zeroed blocks. */
        for (uint32_t i = count; i < need; i++)
            if (!ffs_file_append_block(vol, key, i)) return -3;
        /* Middle blocks of a zero-extension declare full payloads on OFS. */
        if (vol->is_ofs) {
            for (uint32_t i = count; i < need; i++) {
                uint32_t dk = ffs_data_key(vol, key, i);
                if (!dk || dk >= vol->num_blocks) break;
                if (ffs_read_block(vol, dk, g_ffs_data) != 0) break;
                uint32_t rem = new_size - i * payload;
                blk_put32(g_ffs_data, FFS_OFS_LW_SIZE,
                          rem > payload ? payload : rem);
                blk_fix_checksum(g_ffs_data);
                BlockDev_Write(vol->dev, dk, g_ffs_data, 1);
            }
        }
    } else if (count > need) {
        /* Free tail data blocks, then trim the pointer tables. */
        for (uint32_t i = need; i < count; i++) {
            uint32_t dk = ffs_data_key(vol, key, i);
            if (dk && dk < vol->num_blocks) FFS_FreeBlock(vol, dk);
        }
        ffs_trim_ptrs(vol, key, need);
        if (vol->is_ofs) ffs_ofs_fix_tail(vol, key, need, new_size);
    } else if (vol->is_ofs) {
        ffs_ofs_fix_tail(vol, key, need, new_size);
    }

    /* Header: new byte size + fresh datestamp. */
    if (ffs_read_hdr(vol, key) != 0) return -1;
    blk_put32(g_ffs_hdr, FFS_LW_BYTE_SIZE, new_size);
    uint32_t d, m, t;
    ffs_now(&d, &m, &t);
    blk_put32(g_ffs_hdr, FFS_LW_DAYS, d);
    blk_put32(g_ffs_hdr, FFS_LW_MINS, m);
    blk_put32(g_ffs_hdr, FFS_LW_TICKS, t);
    return ffs_write_hdr(vol, key);
}

int32_t FFS_WriteAt(FfsVolume *vol, uint32_t key, uint32_t pos,
                    const void *src, uint32_t len)
{
    if (!vol || !src) return -1;
    FfsEntry e;
    if (FFS_ReadEntry(vol, key, &e) != 0) return -1;
    if (e.sec_type != FFS_ST_FILE && e.sec_type != FFS_ST_LINKFILE)
        return -2;

    /* Seeking past EOF zero-extends the file. */
    if (pos > e.byte_size) {
        if (FFS_SetFileSize(vol, key, pos) != 0) return -3;
        if (FFS_ReadEntry(vol, key, &e) != 0) return -1;
    }

    const uint32_t payload = vol->is_ofs ? FFS_OFS_PAYLOAD : vol->block_size;
    const uint8_t *in = (const uint8_t *)src;
    uint32_t done = 0;
    uint32_t count = ffs_data_count(vol, key);

    while (done < len) {
        uint32_t seq  = (pos + done) / payload;
        uint32_t off  = (pos + done) % payload;
        uint32_t want = payload - off;
        if (want > len - done) want = len - done;

        if (seq >= count) {
            if (!ffs_file_append_block(vol, key, seq)) break;
            count++;
        }
        uint32_t dkey = ffs_data_key(vol, key, seq);
        if (!dkey || dkey >= vol->num_blocks) break;
        if (ffs_read_block(vol, dkey, g_ffs_data) != 0) break;

        if (vol->is_ofs) {
            if (blk_lw(g_ffs_data, FFS_OFS_LW_TYPE) != FFS_T_DATA) break;
            if (off + want > blk_lw(g_ffs_data, FFS_OFS_LW_SIZE))
                blk_put32(g_ffs_data, FFS_OFS_LW_SIZE, off + want);
            memcpy(g_ffs_data + FFS_OFS_DATA_HDR + off, in + done, want);
            blk_fix_checksum(g_ffs_data);
        } else {
            memcpy(g_ffs_data + off, in + done, want);
        }
        if (BlockDev_Write(vol->dev, dkey, g_ffs_data, 1) != 0) break;
        done += want;
    }

    /* Grow byte_size + stamp when the write extended the file. */
    if (done && pos + done > e.byte_size) {
        if (ffs_read_hdr(vol, key) == 0) {
            blk_put32(g_ffs_hdr, FFS_LW_BYTE_SIZE, pos + done);
            uint32_t d, m, t;
            ffs_now(&d, &m, &t);
            blk_put32(g_ffs_hdr, FFS_LW_DAYS, d);
            blk_put32(g_ffs_hdr, FFS_LW_MINS, m);
            blk_put32(g_ffs_hdr, FFS_LW_TICKS, t);
            ffs_write_hdr(vol, key);
        }
    }
    return (int32_t)done;
}

/* ------------------------------------------------------------------
 * Directory mutation
 * ------------------------------------------------------------------ */

/* Split `path` into parent dir key + final component name.
 * Returns 0 on success; dir_key and name are filled. */
static int ffs_split_path(FfsVolume *vol, const char *path,
                          uint32_t *dir_key, char *name, int name_max)
{
    const char *p = path;
    const char *colon = p;
    while (*colon && *colon != ':') colon++;
    if (*colon == ':') p = colon + 1;
    while (*p == '/') p++;

    const char *slash = 0;
    for (const char *q = p; *q; q++)
        if (*q == '/') slash = q;

    char dirpart[128];
    int dn = 0;
    if (slash) {
        for (const char *q = p; q < slash && dn < 126; q++)
            dirpart[dn++] = *q;
        p = slash + 1;
    }
    dirpart[dn] = '\0';

    int nn = 0;
    while (p[nn] && nn < name_max - 1) {
        if (p[nn] == ':' || p[nn] == '/') return -1;
        name[nn] = p[nn];
        nn++;
    }
    name[nn] = '\0';
    if (!nn || p[nn]) return -1;   /* empty or too long */

    uint32_t dk = slash ? FFS_Resolve(vol, dirpart) : vol->root_key;
    if (!dk) return -1;
    FfsEntry e;
    if (FFS_ReadEntry(vol, dk, &e) != 0) return -1;
    if (e.sec_type != FFS_ST_ROOT && e.sec_type != FFS_ST_USERDIR &&
        e.sec_type != FFS_ST_LINKDIR)
        return -1;
    *dir_key = dk;
    return 0;
}

/* Insert `key` into `dir_key`'s hash chain under `name`. */
static int ffs_hash_insert(FfsVolume *vol, uint32_t dir_key,
                           const char *name, uint32_t key)
{
    if (ffs_read_hdr(vol, dir_key) != 0) return -1;
    uint32_t slot = FFS_Hash(name, vol->ht_size);
    uint32_t cur = blk_lw(g_ffs_hdr, FFS_LW_TABLE + slot);
    if (!cur) {
        blk_put32(g_ffs_hdr, FFS_LW_TABLE + slot, key);
        return ffs_write_hdr(vol, dir_key);
    }
    int guard = (int)vol->num_blocks;
    while (cur && cur < vol->num_blocks && guard-- > 0) {
        if (ffs_read_hdr(vol, cur) != 0) return -1;
        uint32_t nxt = blk_lw(g_ffs_hdr, FFS_LW_NEXT_HASH);
        if (!nxt) {
            blk_put32(g_ffs_hdr, FFS_LW_NEXT_HASH, key);
            return ffs_write_hdr(vol, cur);
        }
        cur = nxt;
    }
    return -1;
}

/* Remove `key` from `dir_key`'s hash chain. */
static int ffs_hash_remove(FfsVolume *vol, uint32_t dir_key,
                           const char *name, uint32_t key)
{
    if (ffs_read_hdr(vol, dir_key) != 0) return -1;
    uint32_t slot = FFS_Hash(name, vol->ht_size);
    uint32_t cur = blk_lw(g_ffs_hdr, FFS_LW_TABLE + slot);
    if (!cur) return -1;

    if (cur == key) {
        if (ffs_read_hdr(vol, cur) != 0) return -1;
        uint32_t nxt = blk_lw(g_ffs_hdr, FFS_LW_NEXT_HASH);
        if (ffs_read_hdr(vol, dir_key) != 0) return -1;
        blk_put32(g_ffs_hdr, FFS_LW_TABLE + slot, nxt);
        return ffs_write_hdr(vol, dir_key);
    }

    uint32_t prev = cur;
    int guard = (int)vol->num_blocks;
    while (guard-- > 0) {
        if (ffs_read_hdr(vol, prev) != 0) return -1;
        uint32_t cur2 = blk_lw(g_ffs_hdr, FFS_LW_NEXT_HASH);
        if (!cur2 || cur2 >= vol->num_blocks) return -1;
        if (cur2 == key) {
            if (ffs_read_hdr(vol, cur2) != 0) return -1;
            uint32_t nxt = blk_lw(g_ffs_hdr, FFS_LW_NEXT_HASH);
            if (ffs_read_hdr(vol, prev) != 0) return -1;
            blk_put32(g_ffs_hdr, FFS_LW_NEXT_HASH, nxt);
            return ffs_write_hdr(vol, prev);
        }
        prev = cur2;
    }
    return -1;
}

/* Shared create for files (ST_FILE) and directories (ST_USERDIR). */
static uint32_t ffs_create_entry(FfsVolume *vol, const char *path,
                                 int32_t sec_type)
{
    uint32_t dir_key;
    char name[FFS_MAX_NAME];
    if (ffs_split_path(vol, path, &dir_key, name, sizeof(name)) != 0) {
        kprint("[FFS] create: split_path failed\n");
        return 0;
    }

    /* Duplicate names are illegal. */
    if (FFS_Lookup(vol, dir_key, name)) {
        kprint("[FFS] create: duplicate name\n");
        return 0;
    }

    uint32_t key = FFS_AllocBlock(vol);
    if (!key) { kprint("[FFS] create: alloc failed\n"); return 0; }

    memset(g_ffs_hdr, 0, FFS_BLOCK_BYTES);
    blk_put32(g_ffs_hdr, FFS_LW_TYPE, FFS_T_HEADER);
    blk_put32(g_ffs_hdr, FFS_LW_HEADER_KEY, key);
    /* lw3 stays 0 — dir/file headers don't carry a hash-table size. */
    bstr_from_c(g_ffs_hdr + FFS_LW_NAME * 4, name, 30);
    uint32_t d, m, t;
    ffs_now(&d, &m, &t);
    blk_put32(g_ffs_hdr, FFS_LW_DAYS, d);
    blk_put32(g_ffs_hdr, FFS_LW_MINS, m);
    blk_put32(g_ffs_hdr, FFS_LW_TICKS, t);
    blk_put32(g_ffs_hdr, FFS_LW_PARENT, dir_key);
    blk_put32(g_ffs_hdr, FFS_LW_SEC_TYPE, (uint32_t)sec_type);
    blk_fix_checksum(g_ffs_hdr);
    if (BlockDev_Write(vol->dev, key, g_ffs_hdr, 1) != 0) {
        kprint("[FFS] create: header write failed\n");
        FFS_FreeBlock(vol, key);
        return 0;
    }

    if (ffs_hash_insert(vol, dir_key, name, key) != 0) {
        kprint("[FFS] create: hash_insert failed\n");
        FFS_FreeBlock(vol, key);
        return 0;
    }
    return key;
}

uint32_t FFS_CreateFile(FfsVolume *vol, const char *path)
{
    if (!vol) return 0;
    /* Existing file -> truncate to zero and reuse its header. */
    uint32_t dir_key;
    char name[FFS_MAX_NAME];
    if (ffs_split_path(vol, path, &dir_key, name, sizeof(name)) == 0) {
        uint32_t k = FFS_Lookup(vol, dir_key, name);
        if (k) {
            FfsEntry e;
            if (FFS_ReadEntry(vol, k, &e) != 0) {
                kprint("[FFS] create: entry read failed\n");
                return 0;
            }
            if (e.sec_type != FFS_ST_FILE && e.sec_type != FFS_ST_LINKFILE) {
                kprint("[FFS] create: not a file\n");
                return 0;
            }
            FFS_SetFileSize(vol, k, 0);
            return k;
        }
    }
    return ffs_create_entry(vol, path, FFS_ST_FILE);
}

uint32_t FFS_CreateDir(FfsVolume *vol, const char *path)
{
    if (!vol) return 0;
    return ffs_create_entry(vol, path, FFS_ST_USERDIR);
}

int FFS_Delete(FfsVolume *vol, const char *path)
{
    if (!vol) return -1;
    uint32_t dir_key;
    char name[FFS_MAX_NAME];
    if (ffs_split_path(vol, path, &dir_key, name, sizeof(name)) != 0)
        return -1;

    uint32_t key = FFS_Lookup(vol, dir_key, name);
    if (!key) return -1;
    FfsEntry e;
    if (FFS_ReadEntry(vol, key, &e) != 0) return -1;
    if (e.sec_type == FFS_ST_ROOT) return -1;

    if (e.sec_type == FFS_ST_USERDIR || e.sec_type == FFS_ST_LINKDIR) {
        /* Directories must be empty: no hash-table entry may be set. */
        if (ffs_read_hdr(vol, key) != 0) return -1;
        for (uint32_t i = 0; i < vol->ht_size; i++)
            if (blk_lw(g_ffs_hdr, FFS_LW_TABLE + i)) return -3;
    }

    /* Unlink from the parent hash chain before freeing blocks so a
     * failed bitmap op can't leave a reachable orphan. */
    if (ffs_hash_remove(vol, dir_key, e.name, key) != 0) return -1;

    if (e.sec_type == FFS_ST_FILE || e.sec_type == FFS_ST_LINKFILE) {
        uint32_t count = ffs_data_count(vol, key);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t dk = ffs_data_key(vol, key, i);
            if (dk && dk < vol->num_blocks) FFS_FreeBlock(vol, dk);
        }
        if (ffs_read_hdr(vol, key) == 0)
            ffs_free_ext_chain(vol, blk_lw(g_ffs_hdr, FFS_LW_EXTENSION));
    }
    FFS_FreeBlock(vol, key);
    return 0;
}

int FFS_Rename(FfsVolume *vol, const char *old_path, const char *new_path)
{
    if (!vol || !old_path || !new_path) return -1;
    uint32_t odir, ndir;
    char oname[FFS_MAX_NAME], nname[FFS_MAX_NAME];
    if (ffs_split_path(vol, old_path, &odir, oname, sizeof(oname)) != 0)
        return -1;
    if (ffs_split_path(vol, new_path, &ndir, nname, sizeof(nname)) != 0)
        return -1;

    uint32_t key = FFS_Lookup(vol, odir, oname);
    if (!key) return -1;
    if (FFS_Lookup(vol, ndir, nname)) return -2;   /* target exists */

    FfsEntry e;
    if (FFS_ReadEntry(vol, key, &e) != 0) return -1;
    if (e.sec_type == FFS_ST_ROOT) return -1;

    if (ffs_hash_remove(vol, odir, e.name, key) != 0) return -1;

    /* Patch name + parent in the header. */
    if (ffs_read_hdr(vol, key) != 0) {
        ffs_hash_insert(vol, odir, e.name, key);   /* best-effort undo */
        return -1;
    }
    memset(g_ffs_hdr + FFS_LW_NAME * 4, 0, 32);
    bstr_from_c(g_ffs_hdr + FFS_LW_NAME * 4, nname, 30);
    blk_put32(g_ffs_hdr, FFS_LW_PARENT, ndir);
    uint32_t d, m, t;
    ffs_now(&d, &m, &t);
    blk_put32(g_ffs_hdr, FFS_LW_DAYS, d);
    blk_put32(g_ffs_hdr, FFS_LW_MINS, m);
    blk_put32(g_ffs_hdr, FFS_LW_TICKS, t);
    if (ffs_write_hdr(vol, key) != 0) {
        ffs_hash_insert(vol, odir, e.name, key);
        return -1;
    }
    if (ffs_hash_insert(vol, ndir, nname, key) != 0) {
        /* Restore the old identity as best we can. */
        if (ffs_read_hdr(vol, key) == 0) {
            memset(g_ffs_hdr + FFS_LW_NAME * 4, 0, 32);
            bstr_from_c(g_ffs_hdr + FFS_LW_NAME * 4, e.name, 30);
            blk_put32(g_ffs_hdr, FFS_LW_PARENT, odir);
            ffs_write_hdr(vol, key);
            ffs_hash_insert(vol, odir, e.name, key);
        }
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------
 * Metadata setters
 * ------------------------------------------------------------------ */

int FFS_SetProtect(FfsVolume *vol, uint32_t key, uint32_t prot)
{
    if (ffs_read_hdr(vol, key) != 0) return -1;
    blk_put32(g_ffs_hdr, FFS_LW_PROTECT, prot);
    return ffs_write_hdr(vol, key);
}

int FFS_SetComment(FfsVolume *vol, uint32_t key, const char *comment)
{
    if (ffs_read_hdr(vol, key) != 0) return -1;
    memset(g_ffs_hdr + FFS_LW_COMMENT * 4, 0, 80);
    bstr_from_c(g_ffs_hdr + FFS_LW_COMMENT * 4,
                comment ? comment : "", 79);
    return ffs_write_hdr(vol, key);
}

int FFS_SetDate(FfsVolume *vol, uint32_t key,
                uint32_t days, uint32_t mins, uint32_t ticks)
{
    if (ffs_read_hdr(vol, key) != 0) return -1;
    blk_put32(g_ffs_hdr, FFS_LW_DAYS, days);
    blk_put32(g_ffs_hdr, FFS_LW_MINS, mins);
    blk_put32(g_ffs_hdr, FFS_LW_TICKS, ticks);
    return ffs_write_hdr(vol, key);
}
