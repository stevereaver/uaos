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
