/*
 * blockdev.h — UAOS Block Device Layer
 *
 * Provides a block device abstraction for storage devices like
 * VirtIO block devices. This layer handles sector-level I/O and
 * provides a unified interface for filesystems.
 */

#ifndef UAOS_BLOCKDEV_H
#define UAOS_BLOCKDEV_H

#include <stdint.h>

/* Block device operations */
typedef struct BlockDevOps {
    int (*read)(uint64_t sector, void *buffer, uint32_t num_sectors);
    int (*write)(uint64_t sector, const void *buffer, uint32_t num_sectors);
    uint64_t (*get_capacity)(void);
} BlockDevOps;

/* Block device structure */
typedef struct BlockDev {
    const char *name;           /* Device name (e.g., "virtio0") */
    uint32_t  sector_size;     /* Sector size in bytes (usually 512) */
    uint64_t  num_sectors;     /* Total number of sectors */
    void     *private_data;    /* Driver-specific data */
    const BlockDevOps *ops;    /* Device operations */
    struct BlockDev *next;     /* Next device in list */
} BlockDev;

/* Maximum number of block devices */
#define MAX_BLOCKDEVS 8

/* Register a block device */
int BlockDev_Register(BlockDev *dev);

/* Unregister a block device */
void BlockDev_Unregister(BlockDev *dev);

/* Find a block device by name */
BlockDev *BlockDev_Find(const char *name);

/* Read sectors from a block device */
int BlockDev_Read(BlockDev *dev, uint64_t sector, void *buffer, uint32_t num_sectors);

/* Write sectors to a block device */
int BlockDev_Write(BlockDev *dev, uint64_t sector, const void *buffer, uint32_t num_sectors);

/* Get device capacity in sectors */
uint64_t BlockDev_GetCapacity(BlockDev *dev);

/* Initialize block device layer */
void BlockDev_Init(void);

#endif /* UAOS_BLOCKDEV_H */
