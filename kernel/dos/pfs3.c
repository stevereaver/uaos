/*
 * pfs3.c — UAOS PFS3 Filesystem Driver Implementation
 *
 * Implements PFS3 (Professional File System 3) support for block devices.
 * This is a basic read-only implementation.
 */

#include "pfs3.h"
#include <stdio.h>
#include <string.h>

/* Static allocation for filesystem structures (no malloc in freestanding) */
static Pfs3FS g_pfs3_fs;
static Pfs3File g_pfs3_file;

/* PFS3 signature */
#define PFS3_SIGNATURE 0x50465333  /* 'PFS3' */

/* =========================================================================
 * Helper Functions
 * ========================================================================= */

static int pfs3_is_valid_root(const Pfs3RootBlock *root)
{
    return (root->id == PFS3_SIGNATURE);
}

/* =========================================================================
 * Mount/Unmount
 * ========================================================================= */

Pfs3FS *PFS3_Mount(BlockDev *bdev)
{
    if (!bdev) {
        printf("[PFS3] Invalid block device\n");
        return NULL;
    }

    /* Use static allocation */
    Pfs3FS *fs = &g_pfs3_fs;
    memset(fs, 0, sizeof(Pfs3FS));
    fs->bdev = bdev;
    fs->block_size = 512; /* PFS3 typically uses 512-byte blocks */
    fs->total_blocks = BlockDev_GetCapacity(bdev);

    /* Read root block (usually at block 0 or last block) */
    uint8_t root_sec[512];
    if (BlockDev_Read(bdev, 0, root_sec, 1) != 0) {
        printf("[PFS3] Failed to read root block\n");
        return NULL;
    }

    memcpy(&fs->root, root_sec, sizeof(Pfs3RootBlock));

    /* Validate PFS3 signature */
    if (!pfs3_is_valid_root(&fs->root)) {
        printf("[PFS3] Invalid PFS3 signature\n");
        return NULL;
    }

    /* Parse root block */
    fs->root_block = fs->root.root_block;
    fs->bitmap_start = fs->root.bitmap_start;
    fs->bitmap_blocks = fs->root.bitmap_blocks;

    printf("[PFS3] Mounted: root_block=%u, bitmap_start=%u\n",
           fs->root_block, fs->bitmap_start);

    return fs;
}

void PFS3_Unmount(Pfs3FS *fs)
{
    /* Static allocation - no free needed */
    (void)fs;
}

/* =========================================================================
 * File Operations
 * ========================================================================= */

Pfs3File *PFS3_Open(Pfs3FS *fs, const char *path)
{
    /* TODO: Implement path parsing and directory entry lookup */
    /* For now, return NULL - this is a stub */
    (void)fs;
    (void)path;
    printf("[PFS3] Open: %s (stub)\n", path);
    return NULL;
}

void PFS3_Close(Pfs3File *file)
{
    /* Static allocation - no free needed */
    (void)file;
}

uint32_t PFS3_Read(Pfs3File *file, void *buffer, uint32_t len)
{
    /* TODO: Implement block chain reading */
    (void)file;
    (void)buffer;
    (void)len;
    printf("[PFS3] Read (stub)\n");
    return 0;
}

uint32_t PFS3_Write(Pfs3File *file, const void *buffer, uint32_t len)
{
    /* Write support not implemented yet */
    (void)file;
    (void)buffer;
    (void)len;
    printf("[PFS3] Write not supported\n");
    return 0;
}

void PFS3_Seek(Pfs3File *file, uint32_t pos)
{
    if (file) {
        file->pos = pos;
    }
}

uint32_t PFS3_Size(Pfs3File *file)
{
    if (file) {
        return file->size;
    }
    return 0;
}

int PFS3_ReadDir(Pfs3File *dir, char *name, uint32_t *size, uint8_t *is_dir)
{
    /* TODO: Implement directory entry reading */
    (void)dir;
    (void)name;
    (void)size;
    (void)is_dir;
    printf("[PFS3] ReadDir (stub)\n");
    return 0;
}
