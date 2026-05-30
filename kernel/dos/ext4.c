/*
 * ext4.c — UAOS EXT4 Filesystem Driver Implementation
 *
 * Implements EXT4 filesystem support for block devices.
 * This is a basic read-only implementation.
 */

#include "ext4.h"
#include <stdio.h>
#include <string.h>

/* Static allocation for filesystem structures (no malloc in freestanding) */
static Ext4FS g_ext4_fs;
static Ext4File g_ext4_file;

/* EXT4 magic signature */
#define EXT4_MAGIC 0xEF53

/* =========================================================================
 * Helper Functions
 * ========================================================================= */

static int ext4_is_valid_sb(const Ext4Superblock *sb)
{
    return (sb->magic == EXT4_MAGIC);
}

/* =========================================================================
 * Mount/Unmount
 * ========================================================================= */

Ext4FS *EXT4_Mount(BlockDev *bdev)
{
    if (!bdev) {
        printf("[EXT4] Invalid block device\n");
        return NULL;
    }

    /* Use static allocation */
    Ext4FS *fs = &g_ext4_fs;
    memset(fs, 0, sizeof(Ext4FS));
    fs->bdev = bdev;

    /* Read superblock (at block 1, offset 1024) */
    uint8_t sb_sec[2048];
    if (BlockDev_Read(bdev, 2, sb_sec, 4) != 0) {
        printf("[EXT4] Failed to read superblock\n");
        return NULL;
    }

    memcpy(&fs->sb, sb_sec + 1024, sizeof(Ext4Superblock));

    /* Validate EXT4 signature */
    if (!ext4_is_valid_sb(&fs->sb)) {
        printf("[EXT4] Invalid EXT4 signature\n");
        return NULL;
    }

    /* Parse superblock */
    uint32_t log_block_size = fs->sb.log_block_size;
    fs->block_size = 1024 << log_block_size;
    fs->inode_size = fs->sb.inode_size;
    fs->blocks_per_group = fs->sb.blocks_per_group;
    fs->inodes_per_group = fs->sb.inodes_per_group;

    printf("[EXT4] Mounted: block_size=%u, inode_size=%u, blocks_per_group=%u\n",
           fs->block_size, fs->inode_size, fs->blocks_per_group);

    return fs;
}

void EXT4_Unmount(Ext4FS *fs)
{
    /* Static allocation - no free needed */
    (void)fs;
}

/* =========================================================================
 * File Operations
 * ========================================================================= */

Ext4File *EXT4_Open(Ext4FS *fs, const char *path)
{
    /* TODO: Implement path parsing and inode lookup */
    /* For now, return NULL - this is a stub */
    (void)fs;
    (void)path;
    printf("[EXT4] Open: %s (stub)\n", path);
    return NULL;
}

void EXT4_Close(Ext4File *file)
{
    /* Static allocation - no free needed */
    (void)file;
}

uint32_t EXT4_Read(Ext4File *file, void *buffer, uint32_t len)
{
    /* TODO: Implement block reading with inode block pointers */
    (void)file;
    (void)buffer;
    (void)len;
    printf("[EXT4] Read (stub)\n");
    return 0;
}

uint32_t EXT4_Write(Ext4File *file, const void *buffer, uint32_t len)
{
    /* Write support not implemented yet */
    (void)file;
    (void)buffer;
    (void)len;
    printf("[EXT4] Write not supported\n");
    return 0;
}

void EXT4_Seek(Ext4File *file, uint32_t pos)
{
    if (file) {
        file->pos = pos;
    }
}

uint32_t EXT4_Size(Ext4File *file)
{
    if (file) {
        return file->size;
    }
    return 0;
}

int EXT4_ReadDir(Ext4File *dir, char *name, uint32_t *size, uint8_t *is_dir)
{
    /* TODO: Implement directory entry reading */
    (void)dir;
    (void)name;
    (void)size;
    (void)is_dir;
    printf("[EXT4] ReadDir (stub)\n");
    return 0;
}
