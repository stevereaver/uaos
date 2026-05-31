/*
 * fat32.c — UAOS FAT32 Filesystem Driver Implementation
 *
 * Implements FAT32 filesystem support for block devices.
 * This is a basic read-only implementation.
 */

#include "fat32.h"
#include <stdio.h>
#include <string.h>

/* Static allocation for filesystem structures (no malloc in freestanding) */
static Fat32FS g_fat32_fs;
static Fat32File g_fat32_file;

/* =========================================================================
 * Helper Functions
 * ========================================================================= */

static uint16_t le16_to_cpu(uint16_t val)
{
    return val;
}

static uint32_t le32_to_cpu(uint32_t val)
{
    return val;
}

static int fat32_is_valid_sig(const Fat32BPB *bpb)
{
    return (bpb->boot_sig55aa == 0xAA55);
}

/* =========================================================================
 * FAT Cluster Operations
 * ========================================================================= */

static uint32_t fat32_get_fat_entry(Fat32FS *fs, uint32_t cluster)
{
    /* FAT32 uses 32-bit entries */
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sec = fs->fat_start + (fat_offset / fs->bytes_per_sec);
    uint32_t fat_off = fat_offset % fs->bytes_per_sec;
    
    uint8_t sector[512];
    if (BlockDev_Read(fs->bdev, fat_sec, sector, 1) != 0) {
        printf("[FAT32] Failed to read FAT sector %u\n", fat_sec);
        return 0xFFFFFFFF; /* Invalid cluster */
    }
    
    uint32_t entry = *(uint32_t*)(sector + fat_off);
    return le32_to_cpu(entry) & 0x0FFFFFFF; /* Mask high 4 bits */
}

/* =========================================================================
 * Mount/Unmount
 * ========================================================================= */

Fat32FS *FAT32_Mount(BlockDev *bdev)
{
    if (!bdev) {
        printf("[FAT32] Invalid block device\n");
        return NULL;
    }

    /* Use static allocation */
    Fat32FS *fs = &g_fat32_fs;
    memset(fs, 0, sizeof(Fat32FS));
    fs->bdev = bdev;

    /* Read boot sector */
    uint8_t boot_sec[512];
    if (BlockDev_Read(bdev, 0, boot_sec, 1) != 0) {
        printf("[FAT32] Failed to read boot sector\n");
        return NULL;
    }

    memcpy(&fs->bpb, boot_sec, sizeof(Fat32BPB));

    /* Validate FAT32 signature */
    if (!fat32_is_valid_sig(&fs->bpb)) {
        printf("[FAT32] Invalid boot signature\n");
        return NULL;
    }

    /* Parse BPB */
    fs->bytes_per_sec = le16_to_cpu(fs->bpb.bytes_per_sec);
    fs->sec_per_clus = fs->bpb.sec_per_clus;
    fs->cluster_size = fs->bytes_per_sec * fs->sec_per_clus;
    fs->root_cluster = le32_to_cpu(fs->bpb.root_clus);
    
    /* Calculate FAT and data start sectors */
    uint32_t rsvd_sec = le16_to_cpu(fs->bpb.rsvd_sec_cnt);
    uint32_t num_fats = fs->bpb.num_fats;
    uint32_t fat_sz32 = le32_to_cpu(fs->bpb.fat_sz32);
    
    fs->fat_start = rsvd_sec;
    fs->data_start = rsvd_sec + (num_fats * fat_sz32);

    printf("[FAT32] Mounted: bytes/sec=%u, sec/clus=%u, root_clus=%u\n",
           fs->bytes_per_sec, fs->sec_per_clus, fs->root_cluster);

    return fs;
}

void FAT32_Unmount(Fat32FS *fs)
{
    /* Static allocation - no free needed */
    (void)fs;
}

/* =========================================================================
 * File Operations
 * ========================================================================= */

Fat32File *FAT32_Open(Fat32FS *fs, const char *path)
{
    /* TODO: Implement path parsing and directory entry lookup */
    /* For now, return NULL - this is a stub */
    (void)fs;
    (void)path;
    printf("[FAT32] Open: %s (stub)\n", path);
    return NULL;
}

void FAT32_Close(Fat32File *file)
{
    /* Static allocation - no free needed */
    (void)file;
}

uint32_t FAT32_Read(Fat32File *file, void *buffer, uint32_t len)
{
    /* TODO: Implement cluster chain reading */
    (void)file;
    (void)buffer;
    (void)len;
    printf("[FAT32] Read (stub)\n");
    return 0;
}

uint32_t FAT32_Write(Fat32File *file, const void *buffer, uint32_t len)
{
    /* Write support not implemented yet */
    (void)file;
    (void)buffer;
    (void)len;
    printf("[FAT32] Write not supported\n");
    return 0;
}

void FAT32_Seek(Fat32File *file, uint32_t pos)
{
    if (file) {
        file->pos = pos;
    }
}

uint32_t FAT32_Size(Fat32File *file)
{
    if (file) {
        return file->size;
    }
    return 0;
}

int FAT32_ReadDir(Fat32File *dir, char *name, uint32_t *size, uint8_t *is_dir)
{
    /* TODO: Implement directory entry reading */
    (void)dir;
    (void)name;
    (void)size;
    (void)is_dir;
    printf("[FAT32] ReadDir (stub)\n");
    return 0;
}

/* =========================================================================
 * Format
 * ========================================================================= */

int FAT32_Format(BlockDev *bdev)
{
    if (!bdev) {
        printf("[FAT32] Invalid block device\n");
        return -1;
    }

    uint64_t total_sectors = BlockDev_GetCapacity(bdev);
    if (total_sectors == 0) {
        printf("[FAT32] Zero capacity device\n");
        return -1;
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

    /* Large zero buffer for batch writes (max 128 sectors = 64KB) */
    static uint8_t zero_buf[65536];
    memset(zero_buf, 0, sizeof(zero_buf));

    /* --- 1. Zero out boot sector area (0..rsvd_sec_cnt-1) --- */
    if (BlockDev_Write(bdev, 0, zero_buf, rsvd_sec_cnt) != 0) {
        printf("[FAT32] Failed to zero reserved area\n");
        return -1;
    }

    /* --- 2. Build Boot Sector (BPB) --- */
    Fat32BPB bpb;
    memset(&bpb, 0, sizeof(bpb));

    bpb.jmp_boot[0] = 0xEB;
    bpb.jmp_boot[1] = 0x58;
    bpb.jmp_boot[2] = 0x90;
    memcpy(bpb.oem_name, "UAOS    ", 8);
    bpb.bytes_per_sec = bytes_per_sec;
    bpb.sec_per_clus  = sec_per_clus;
    bpb.rsvd_sec_cnt  = rsvd_sec_cnt;
    bpb.num_fats      = num_fats;
    bpb.root_ent_cnt  = 0;
    bpb.tot_sec16     = 0;
    bpb.media         = 0xF8;
    bpb.fat_sz16      = 0;
    bpb.sec_per_trk   = 63;
    bpb.num_heads     = 255;
    bpb.hidd_sec      = 0;
    bpb.tot_sec32     = (uint32_t)total_sectors;
    bpb.fat_sz32      = fat_sz;
    bpb.ext_flags     = 0;
    bpb.fs_ver        = 0;
    bpb.root_clus     = root_clus;
    bpb.fs_info       = 1;
    bpb.bk_boot_sec   = 6;
    bpb.drv_num       = 0x80;
    bpb.reserved1     = 0;
    bpb.boot_sig      = 0x29;
    bpb.vol_id        = 0x12345678;
    memcpy(bpb.vol_label, "UAOS       ", 11);
    memcpy(bpb.fs_type, "FAT32   ", 8);
    bpb.boot_sig55aa  = 0xAA55;

    if (BlockDev_Write(bdev, 0, &bpb, 1) != 0) {
        printf("[FAT32] Failed to write boot sector\n");
        return -1;
    }

    /* --- 3. FSINFO Sector (sector 1) --- */
    uint8_t fsinfo[512];
    memset(fsinfo, 0, sizeof(fsinfo));
    *(uint32_t *)(fsinfo + 0)   = 0x41615252;  /* lead sig */
    *(uint32_t *)(fsinfo + 484) = 0x61417272;  /* struc sig */
    *(uint32_t *)(fsinfo + 488) = 0xFFFFFFFF;  /* free count (unknown) */
    *(uint32_t *)(fsinfo + 492) = 0xFFFFFFFF;  /* next free (unknown) */
    *(uint32_t *)(fsinfo + 508) = 0xAA550000;  /* trail sig */
    if (BlockDev_Write(bdev, 1, fsinfo, 1) != 0) {
        printf("[FAT32] Failed to write FSINFO\n");
        return -1;
    }

    /* --- 4. Zero FATs (batch up to 128 sectors per write) --- */
    for (uint32_t f = 0; f < num_fats; f++) {
        uint32_t fat_start = rsvd_sec_cnt + (f * fat_sz);
        uint32_t remain = fat_sz;
        uint32_t s = 0;
        while (remain > 0) {
            uint32_t batch = (remain > 128) ? 128 : remain;
            if (BlockDev_Write(bdev, fat_start + s, zero_buf, batch) != 0) {
                printf("[FAT32] Failed to zero FAT%u at sector %u\n", f, s);
                return -1;
            }
            s += batch;
            remain -= batch;
        }
    }

    /* --- 5. Write initial FAT entries --- */
    uint8_t fat_init[512];
    memset(fat_init, 0, sizeof(fat_init));
    /* Entry 0: media byte in low nibble, high bits set */
    *(uint32_t *)(fat_init + 0) = 0x0FFFFF00 | bpb.media;
    /* Entry 1: EOC marker */
    *(uint32_t *)(fat_init + 4) = 0x0FFFFFFF;
    /* Entry 2 (root dir): EOC marker */
    *(uint32_t *)(fat_init + 8) = 0x0FFFFFFF;

    for (uint32_t f = 0; f < num_fats; f++) {
        uint32_t fat_start = rsvd_sec_cnt + (f * fat_sz);
        if (BlockDev_Write(bdev, fat_start, fat_init, 1) != 0) {
            printf("[FAT32] Failed to write initial FAT%u\n", f);
            return -1;
        }
    }

    /* --- 6. Zero root directory cluster --- */
    uint32_t data_start = rsvd_sec_cnt + (num_fats * fat_sz);
    uint32_t root_sec   = data_start + ((root_clus - 2) * sec_per_clus);
    if (BlockDev_Write(bdev, root_sec, zero_buf, sec_per_clus) != 0) {
        printf("[FAT32] Failed to zero root dir\n");
        return -1;
    }

    /* --- 7. Write backup boot sector (sector 6) --- */
    if (BlockDev_Write(bdev, 6, &bpb, 1) != 0) {
        printf("[FAT32] Failed to write backup boot sector\n");
        return -1;
    }

    printf("[FAT32] Format complete.\n");
    return 0;
}
