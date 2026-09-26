/* ffs.c — UAOS Amiga OFS/FFS filesystem driver (read-only)
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
    out->byte_size = blk_lw(g_ffs_hdr, 81);
    out->protect = blk_lw(g_ffs_hdr, 80);
    out->parent_key = blk_lw(g_ffs_hdr, FFS_LW_PARENT);
    out->ext_key = blk_lw(g_ffs_hdr, FFS_LW_EXTENSION);
    out->next_hash = blk_lw(g_ffs_hdr, FFS_LW_NEXT_HASH);
    out->high_seq = blk_lw(g_ffs_hdr, FFS_LW_HIGH_SEQ);
    out->days = blk_lw(g_ffs_hdr, FFS_LW_DAYS);
    out->mins = blk_lw(g_ffs_hdr, FFS_LW_MINS);
    out->ticks = blk_lw(g_ffs_hdr, FFS_LW_TICKS);
    bstr_to_c(g_ffs_hdr + FFS_LW_NAME * 4, out->name, sizeof(out->name));

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
