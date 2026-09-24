/*
 * fat32.c — UAOS FAT32 Filesystem Driver Implementation
 *
 * Implements read/write FAT32 filesystem support for block devices.
 * Supports: file open/create/read/write, directory create/delete/list,
 * cluster allocation/deallocation, and path traversal.
 */

#include "fat32.h"
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
        if (BlockDev_Write(fs->bdev, sec + i,
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
        if (BlockDev_Write(fs->bdev, sec + s, g_cluster_buf, batch) != 0)
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
            if (BlockDev_Write(fs->bdev, base + fat_sec - fs->fat_start,
                               g_sector_buf, 1) != 0) return -1;
            if (BlockDev_Write(fs->bdev, base + fat_sec + 1 - fs->fat_start,
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
        if (BlockDev_Write(fs->bdev, base + (fat_sec - fs->fat_start),
                           g_sector_buf, 1) != 0) return -1;
    }
    return 0;
}

/* =========================================================================
 * Cluster allocation / freeing
 * ========================================================================= */

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
                if (fat32_set_fat_entry(fs, cluster, FAT32_EOC) != 0)
                    return 0;
                /* Zero the cluster data */
                if (fat32_zero_cluster(fs, cluster) != 0)
                    return 0;
                return cluster;
            }
        }
        (void)total_fat_sectors;
    }
    printf("[FAT32] Disk full — no free clusters\n");
    return 0;
}

/* Free an entire cluster chain starting at start_cluster */
static void fat32_free_chain(Fat32FS *fs, uint32_t start_cluster)
{
    uint32_t cluster = start_cluster;
    while (cluster >= 2 && !FAT32_IS_EOC(cluster)) {
        uint32_t next = fat32_get_fat_entry(fs, cluster);
        fat32_set_fat_entry(fs, cluster, 0);
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
 * Name conversion (8.3 <-> display name)
 * ========================================================================= */

static void fat32_name_to_83(const char *name, char *out83)
{
    /* Convert "FILE.TXT" to FAT 8.3 format (11 chars, space-padded) */
    for (int i = 0; i < 11; i++) out83[i] = ' ';
    out83[11] = '\0';

    int i = 0, pos = 0;
    /* Copy base name (up to 8 chars, stop at '.') */
    while (name[i] && pos < 8) {
        if (name[i] == '.') break;
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out83[pos++] = c;
        i++;
    }
    /* Skip to dot or end */
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') i++;
    /* Copy extension (up to 3 chars) */
    pos = 8;
    while (name[i] && pos < 11) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out83[pos++] = c;
        i++;
    }
}

static void fat32_83_to_name(const char *name83, char *out)
{
    int i, j = 0;
    for (i = 0; i < 8 && name83[i] != ' '; i++) {
        char c = name83[i];
        /* Keep case as stored (could be upper or lower depending on NT flags) */
        out[j++] = c;
    }
    if (name83[8] != ' ') {
        out[j++] = '.';
        for (i = 8; i < 11 && name83[i] != ' '; i++)
            out[j++] = name83[i];
    }
    out[j] = '\0';
}

/* =========================================================================
 * Directory entry helpers
 * ========================================================================= */

/* Directory entry size is always 32 bytes */
#define FAT32_DIR_ENTRY_SIZE 32

/* Parse a raw 32-byte directory entry into fields */
static void fat32_parse_dir_entry(const uint8_t *de,
                                  char *name83,
                                  uint8_t *attr,
                                  uint32_t *cluster,
                                  uint32_t *size)
{
    if (name83)  memcpy(name83, de, 11);
    if (attr)    *attr = de[11];
    if (cluster) *cluster = le16(&de[26]) | ((uint32_t)le16(&de[20]) << 16);
    if (size)    *size = le32(&de[28]);
}

/* Find a directory entry by 8.3 name within a directory cluster chain.
 * On success: fills *out_cluster (first cluster), *out_size, *out_is_dir,
 *   *out_dir_sector (sector containing the entry), *out_dir_offset (byte offset).
 * Returns 1 on found, 0 on not found. */
static int fat32_find_in_dir(Fat32FS *fs, uint32_t dir_cluster,
                             const char *name83,
                             uint32_t *out_cluster, uint32_t *out_size,
                             uint8_t *out_attr,
                             uint32_t *out_dir_sector, uint32_t *out_dir_offset)
{
    uint32_t cluster = dir_cluster;
    uint32_t ents_per_cluster = fs->cluster_size / FAT32_DIR_ENTRY_SIZE;

    while (cluster >= 2 && !FAT32_IS_EOC(cluster)) {
        if (fat32_read_cluster(fs, cluster, g_cluster_buf) != 0) return 0;
        uint32_t sec = fat32_cluster_to_sector(fs, cluster);

        for (uint32_t e = 0; e < ents_per_cluster; e++) {
            uint8_t *de = &g_cluster_buf[e * FAT32_DIR_ENTRY_SIZE];
            if (de[0] == 0x00) return 0;  /* end of directory */
            if (de[0] == 0xE5) continue;   /* deleted entry */
            uint8_t attr = de[11];
            if ((attr & 0x0F) == 0x0F) continue;  /* LFN entry */
            if ((attr & 0x18) == 0x08) continue;  /* volume label */

            if (memcmp(de, name83, 11) == 0) {
                if (out_cluster) *out_cluster = le16(&de[26]) | ((uint32_t)le16(&de[20]) << 16);
                if (out_size)    *out_size = le32(&de[28]);
                if (out_attr)    *out_attr = attr;
                if (out_dir_sector) *out_dir_sector = sec + (e * FAT32_DIR_ENTRY_SIZE) / fs->bytes_per_sec;
                if (out_dir_offset) *out_dir_offset = (e * FAT32_DIR_ENTRY_SIZE) % fs->bytes_per_sec;
                return 1;
            }
        }
        cluster = fat32_get_fat_entry(fs, cluster);
    }
    return 0;
}

/* Find a free slot in a directory for a new entry (32 bytes).
 * If the directory is full, extend it by one cluster.
 * On success: returns 1, fills *out_sector and *out_offset.
 * Returns 0 on failure. */
static int fat32_find_free_dir_slot(Fat32FS *fs, uint32_t dir_cluster,
                                    uint32_t *out_sector, uint32_t *out_offset)
{
    uint32_t cluster = dir_cluster;
    uint32_t ents_per_cluster = fs->cluster_size / FAT32_DIR_ENTRY_SIZE;
    uint32_t last_cluster = cluster;

    while (cluster >= 2 && !FAT32_IS_EOC(cluster)) {
        if (fat32_read_cluster(fs, cluster, g_cluster_buf) != 0) return 0;
        uint32_t sec = fat32_cluster_to_sector(fs, cluster);

        for (uint32_t e = 0; e < ents_per_cluster; e++) {
            uint8_t *de = &g_cluster_buf[e * FAT32_DIR_ENTRY_SIZE];
            if (de[0] == 0x00 || de[0] == 0xE5) {
                *out_sector = sec + (e * FAT32_DIR_ENTRY_SIZE) / fs->bytes_per_sec;
                *out_offset = (e * FAT32_DIR_ENTRY_SIZE) % fs->bytes_per_sec;
                return 1;
            }
        }
        last_cluster = cluster;
        cluster = fat32_get_fat_entry(fs, cluster);
    }

    /* Directory is full — extend by one cluster */
    uint32_t newc = fat32_extend_chain(fs, last_cluster);
    if (newc == 0) return 0;
    /* fat32_alloc_cluster already zeroed it, so first entry is 0x00 = end */
    *out_sector = fat32_cluster_to_sector(fs, newc);
    *out_offset = 0;
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
        if (BlockDev_Write(fs->bdev, sector, g_sector_buf, 1) != 0) continue;
        if (BlockDev_Read(fs->bdev, sector, g_sector_buf2, 1) != 0) continue;
        if (memcmp(&g_sector_buf2[offset], entry, FAT32_DIR_ENTRY_SIZE) == 0)
            return 0;
    }
    printf("[FAT32] Directory entry write did not persist at sector %u\n", sector);
    return -1;
}

/* Build a raw 32-byte directory entry */
static void fat32_build_dir_entry(uint8_t *de, const char *name83,
                                  uint8_t attr, uint32_t cluster,
                                  uint32_t size)
{
    memset(de, 0, FAT32_DIR_ENTRY_SIZE);
    memcpy(de, name83, 11);
    de[11] = attr;
    put_le16(&de[20], (uint16_t)(cluster >> 16));    /* fst_clus_hi */
    put_le16(&de[26], (uint16_t)(cluster & 0xFFFF)); /* fst_clus_lo */
    put_le32(&de[28], size);                          /* file_size */
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

/* Walk a path like "dir1/dir2/file" from a starting directory cluster.
 * For each component except the last, descend into the directory.
 * On success: fills *out_dir_cluster (parent dir cluster), *out_name83
 *   (the final component in 8.3 format), and optionally the entry info.
 * Returns 1 on success (final component found), 0 on not found,
 *   -1 on error (intermediate dir not found). */
static int fat32_walk_path(Fat32FS *fs, const char *path,
                           uint32_t start_cluster,
                           uint32_t *out_dir_cluster,
                           char *out_name83,
                           uint32_t *out_file_cluster,
                           uint32_t *out_file_size,
                           uint8_t *out_file_attr,
                           uint32_t *out_entry_sector,
                           uint32_t *out_entry_offset)
{
    /* Copy path to a local buffer for tokenization */
    char tmp[128];
    int i = 0;
    while (i < 127 && path[i]) { tmp[i] = path[i]; i++; }
    tmp[i] = '\0';

    /* Strip leading slashes */
    char *p = tmp;
    while (*p == '/' || *p == '\\') p++;

    uint32_t dir_cluster = start_cluster;
    char *component = p;
    char *next_slash;

    while (1) {
        /* Find the next slash */
        next_slash = p;
        while (*next_slash && *next_slash != '/' && *next_slash != '\\')
            next_slash++;

        if (*next_slash == '\0') {
            /* Final component */
            if (*p == '\0') {
                /* Empty path = root directory itself */
                if (out_dir_cluster) *out_dir_cluster = dir_cluster;
                if (out_name83) {
                    for (int j = 0; j < 11; j++) out_name83[j] = ' ';
                    out_name83[11] = '\0';
                }
                return 1;
            }
            /* Convert to 8.3 and search */
            char name83[12];
            fat32_name_to_83(p, name83);
            if (out_dir_cluster) *out_dir_cluster = dir_cluster;
            if (out_name83) memcpy(out_name83, name83, 12);

            return fat32_find_in_dir(fs, dir_cluster, name83,
                                     out_file_cluster, out_file_size,
                                     out_file_attr,
                                     out_entry_sector, out_entry_offset);
        }

        /* Intermediate component — must be a directory */
        *next_slash = '\0';
        char name83[12];
        fat32_name_to_83(p, name83);
        uint32_t sub_cluster = 0;
        uint8_t sub_attr = 0;
        int found = fat32_find_in_dir(fs, dir_cluster, name83,
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
    if (fs->total_clusters == 0) {
        printf("[FAT32] No data clusters\n");
        return NULL;
    }

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

    int found = fat32_walk_path(fs, rel, fs->root_cluster,
                                NULL, NULL,
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
    char name83[12];

    int found = fat32_walk_path(fs, rel, fs->root_cluster,
                                &dir_cluster, name83,
                                &existing_cluster, &existing_size,
                                &existing_attr, &entry_sec, &entry_off);

    if (found == 1) {
        /* File exists — if it's a directory, can't truncate */
        if (existing_attr & FAT32_ATTR_DIRECTORY) return NULL;
        /* Free existing cluster chain and reset entry */
        if (existing_cluster >= 2)
            fat32_free_chain(fs, existing_cluster);
        /* Update dir entry: cluster=0, size=0 */
        uint8_t entry[32];
        fat32_build_dir_entry(entry, name83, 0, 0, 0);
        fat32_write_dir_entry(fs, entry_sec, entry_off, entry);
    } else if (found == 0) {
        /* File doesn't exist — need parent dir + name */
        /* Re-walk to get parent dir and name */
        char tmp[128];
        int i = 0;
        while (i < 127 && rel[i]) { tmp[i] = rel[i]; i++; }
        tmp[i] = '\0';

        /* Find last slash to split path */
        char *last_slash = NULL;
        for (char *s = tmp; *s; s++)
            if (*s == '/' || *s == '\\') last_slash = s;

        if (last_slash) {
            *last_slash = '\0';
            /* Walk to parent directory */
            uint32_t pcl = 0;
            uint8_t parent_attr = 0;
            int pfound = fat32_walk_path(fs, tmp, fs->root_cluster,
                                         NULL, NULL, &pcl, NULL, &parent_attr,
                                         NULL, NULL);
            if (pfound != 1 || !(parent_attr & FAT32_ATTR_DIRECTORY)) return NULL;
            dir_cluster = pcl;
            fat32_name_to_83(last_slash + 1, name83);
        } else {
            dir_cluster = fs->root_cluster;
            fat32_name_to_83(tmp, name83);
        }

        /* Find a free directory slot */
        if (!fat32_find_free_dir_slot(fs, dir_cluster,
                                      &entry_sec, &entry_off))
            return NULL;

        /* Write the new (empty) directory entry */
        uint8_t entry[32];
        fat32_build_dir_entry(entry, name83, 0, 0, 0);
        if (fat32_write_dir_entry(fs, entry_sec, entry_off, entry) != 0)
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
            BlockDev_Write(file->fs->bdev, file->dir_sector,
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

        /* Read current cluster if we're at the start of it */
        if (file->offset == 0) {
            if (fat32_read_cluster(file->fs, file->cluster,
                                   g_cluster_buf) != 0) break;
        }

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

int FAT32_ReadDir(Fat32File *dir, char *name, uint32_t *size, uint8_t *is_dir)
{
    if (!dir || !dir->fs || !dir->is_dir) return 0;
    Fat32FS *fs = dir->fs;
    uint32_t cluster_size = fs->cluster_size;
    uint32_t ents_per_cluster = cluster_size / FAT32_DIR_ENTRY_SIZE;

    /* Initialize iterator on first call (iter_cluster == 0 means start) */
    if (dir->iter_cluster == 0) {
        dir->iter_cluster = dir->start_cluster;
        dir->iter_offset = 0;
    }

    while (dir->iter_cluster >= 2 && !FAT32_IS_EOC(dir->iter_cluster)) {
        /* Read the current cluster if we're at offset 0 within it */
        if (dir->iter_offset == 0) {
            if (fat32_read_cluster(fs, dir->iter_cluster, g_cluster_buf) != 0)
                return 0;
        }

        /* Scan entries in the current cluster from iter_offset */
        uint32_t start_ent = dir->iter_offset / FAT32_DIR_ENTRY_SIZE;
        for (uint32_t e = start_ent; e < ents_per_cluster; e++) {
            uint8_t *de = &g_cluster_buf[e * FAT32_DIR_ENTRY_SIZE];

            /* Advance iterator past this entry */
            dir->iter_offset = (e + 1) * FAT32_DIR_ENTRY_SIZE;

            if (de[0] == 0x00) {
                /* End of directory */
                dir->iter_cluster = 0;
                return 0;
            }
            if (de[0] == 0xE5) continue;  /* deleted */
            uint8_t attr = de[11];
            if ((attr & 0x0F) == 0x0F) continue;  /* LFN */
            if ((attr & 0x18) == 0x08) continue;  /* volume label */

            /* Skip "." and ".." entries */
            if (de[0] == '.' && de[1] == ' ' &&
                de[2] == ' ' && de[3] == ' ') continue;
            if (de[0] == '.' && de[1] == '.' &&
                de[2] == ' ' && de[3] == ' ') continue;

            /* Found a valid entry */
            char name83[12];
            memcpy(name83, de, 11);
            name83[11] = '\0';
            fat32_83_to_name(name83, name);

            if (size)   *size = le32(&de[28]);
            if (is_dir) *is_dir = (attr & FAT32_ATTR_DIRECTORY) ? 1 : 0;
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

    /* Walk path to find parent dir and final component name */
    char tmp[128];
    int i = 0;
    while (i < 127 && rel[i]) { tmp[i] = rel[i]; i++; }
    tmp[i] = '\0';

    /* Find last slash to split path */
    char *last_slash = NULL;
    for (char *s = tmp; *s; s++)
        if (*s == '/' || *s == '\\') last_slash = s;

    uint32_t parent_cluster = fs->root_cluster;
    char name83[12];

    if (last_slash) {
        *last_slash = '\0';
        /* Walk to parent directory */
        uint32_t pcl = 0;
        uint8_t parent_attr = 0;
        int pfound = fat32_walk_path(fs, tmp, fs->root_cluster,
                                     NULL, NULL, &pcl, NULL, &parent_attr,
                                     NULL, NULL);
        if (pfound != 1 || !(parent_attr & FAT32_ATTR_DIRECTORY)) return -1;
        parent_cluster = pcl;
        fat32_name_to_83(last_slash + 1, name83);
    } else {
        fat32_name_to_83(tmp, name83);
    }

    /* Check if it already exists */
    if (fat32_find_in_dir(fs, parent_cluster, name83,
                          NULL, NULL, NULL, NULL, NULL))
        return -1;  /* already exists */

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

    /* ".." entry — points to parent (name = "..         ") */
    uint8_t *dotdot_entry = g_cluster_buf + 32;
    memset(dotdot_entry, ' ', 11);
    dotdot_entry[0] = '.';
    dotdot_entry[1] = '.';
    dotdot_entry[11] = FAT32_ATTR_DIRECTORY;
    put_le16(&dotdot_entry[20], (uint16_t)(parent_cluster >> 16));
    put_le16(&dotdot_entry[26], (uint16_t)(parent_cluster & 0xFFFF));
    put_le32(&dotdot_entry[28], 0);

    /* Write the directory cluster — write all sectors at once using
     * the DMA-safe g_cluster_buf. */
    uint32_t dir_sec = fat32_cluster_to_sector(fs, new_cluster);
    if (BlockDev_Write(fs->bdev, dir_sec, g_cluster_buf, fs->sec_per_clus) != 0) {
        printf("[FAT32] Failed to write new directory cluster\n");
        return -1;
    }

    /* Find a free slot in the parent directory and write the new entry */
    uint32_t entry_sec = 0, entry_off = 0;
    if (!fat32_find_free_dir_slot(fs, parent_cluster, &entry_sec, &entry_off))
        return -1;

    uint8_t entry[32];
    fat32_build_dir_entry(entry, name83, FAT32_ATTR_DIRECTORY,
                          new_cluster, 0);
    if (fat32_write_dir_entry(fs, entry_sec, entry_off, entry) != 0)
        return -1;

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
                                NULL, NULL,
                                &file_cluster, &file_size, &file_attr,
                                &entry_sec, &entry_off);
    if (found != 1) return -1;

    /* If it's a directory, check that it's empty (only . and .. entries) */
    if (file_attr & FAT32_ATTR_DIRECTORY) {
        if (file_cluster < 2) return -1;
        Fat32File *dir = FAT32_Open(fs, path);
        if (!dir) return -1;
        char name[32];
        uint32_t size;
        uint8_t is_dir;
        while (FAT32_ReadDir(dir, name, &size, &is_dir)) {
            /* Any entry other than . and .. means non-empty */
            FAT32_Close(dir);
            return -1;
        }
        FAT32_Close(dir);
    }

    /* Free the cluster chain */
    if (file_cluster >= 2)
        fat32_free_chain(fs, file_cluster);

    /* Mark the directory entry as deleted (0xE5) */
    if (BlockDev_Read(fs->bdev, entry_sec, g_sector_buf, 1) != 0)
        return -1;
    g_sector_buf[entry_off] = 0xE5;
    if (BlockDev_Write(fs->bdev, entry_sec, g_sector_buf, 1) != 0)
        return -1;

    return 0;
}

/* =========================================================================
 * Volume statistics
 * ========================================================================= */

void FAT32_GetVolumeStats(Fat32FS *fs, uint32_t *total_bytes, uint32_t *used_bytes)
{
    if (!fs) return;
    uint32_t total = fs->total_clusters * fs->cluster_size;
    uint32_t used = 0;

    /* Count used clusters by scanning the FAT */
    uint32_t fat_sz = fs->bpb.fat_sz32;
    for (uint32_t sec = 0; sec < fat_sz; sec++) {
        if (BlockDev_Read(fs->bdev, fs->fat_start + sec,
                          g_sector_buf, 1) != 0) break;
        uint32_t ents_per_sec = fs->bytes_per_sec / 4;
        for (uint32_t e = 0; e < ents_per_sec; e++) {
            uint32_t cluster = sec * ents_per_sec + e;
            if (cluster < 2 || cluster >= fs->total_clusters + 2)
                continue;
            uint32_t val = le32(&g_sector_buf[e * 4]) & 0x0FFFFFFF;
            if (val != 0) used++;
        }
    }

    if (total_bytes) *total_bytes = total;
    if (used_bytes)  *used_bytes = used * fs->cluster_size;
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

    /* Approximate data sectors and cluster count */
    uint32_t data_sectors = (uint32_t)(total_sectors - rsvd_sec_cnt);
    uint32_t total_clusters = data_sectors / sec_per_clus;
    /* FAT size = ceil(total_clusters * 4 / 512) */
    uint32_t fat_sz = (total_clusters + 127) / 128;
    if (fat_sz < 1) fat_sz = 1;

    /* Re-calculate with actual FAT size */
    data_sectors = (uint32_t)total_sectors - rsvd_sec_cnt - (num_fats * fat_sz);
    total_clusters = data_sectors / sec_per_clus;

    /* Re-calculate FAT size to cover all clusters */
    fat_sz = (total_clusters + 127) / 128;
    if (fat_sz < 1) fat_sz = 1;

    printf("[FAT32] Format: %u sectors, clus=%u, FAT=%u sectors\n",
           (uint32_t)total_sectors, sec_per_clus, fat_sz);

    /* Use the global 4K-aligned g_cluster_buf for all zeroing.
     * This keeps DMA buffers in a known, large, aligned region. */
    memset(g_cluster_buf, 0, sizeof(g_cluster_buf));

    /* --- 1. Zero out boot sector area (0..rsvd_sec_cnt-1) --- */
    if (BlockDev_Write(bdev, 0, g_cluster_buf, rsvd_sec_cnt) != 0) {
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

    if (BlockDev_Write(bdev, 0, g_sector_buf, 1) != 0) {
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
    if (BlockDev_Write(bdev, 1, g_sector_buf, 1) != 0) {
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
            /* Keep each write <= 64 sectors (32 KB) to stay safely inside
             * the VirtIO/scsi data buffers and device limits. */
            uint32_t batch = (remain > 64) ? 64 : remain;
            if (BlockDev_Write(bdev, fat_start + s, g_cluster_buf, batch) != 0) {
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
        if (BlockDev_Write(bdev, fat_start, g_sector_buf, 1) != 0) {
            printf("[FAT32] Failed to write initial FAT%u at sector %u (rc=%d)\n",
                   f, fat_start, 0);
            return -7;
        }
    }

    /* --- 6. Zero root directory cluster --- */
    uint32_t data_start = rsvd_sec_cnt + (num_fats * fat_sz);
    uint32_t root_sec   = data_start + ((root_clus - 2) * sec_per_clus);
    if (BlockDev_Write(bdev, root_sec, g_cluster_buf, sec_per_clus) != 0) {
        printf("[FAT32] Failed to zero root dir\n");
        return -8;
    }

    /* --- 7. Write backup boot sector (sector 6) ---
     * Restore the BPB we saved earlier and write it to the backup location. */
    memcpy(g_sector_buf, g_sector_buf2, 512);
    if (BlockDev_Write(bdev, 6, g_sector_buf, 1) != 0) {
        printf("[FAT32] Failed to write backup boot sector\n");
        return -9;
    }

    printf("[FAT32] Format complete.\n");
    return 0;
}
