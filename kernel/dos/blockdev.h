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

/* Forward declaration */
struct BlockDev;

/* Block device operations */
typedef struct BlockDevOps {
    int (*read)(struct BlockDev *dev, uint64_t sector, void *buffer, uint32_t num_sectors);
    int (*write)(struct BlockDev *dev, uint64_t sector, const void *buffer, uint32_t num_sectors);
    uint64_t (*get_capacity)(struct BlockDev *dev);
} BlockDevOps;

/* Block device structure */
typedef struct BlockDev {
    const char *name;           /* Device name (e.g., "virtio0") */
    const char *display_name;   /* Display name (e.g., "DH0:") */
    uint32_t  sector_size;     /* Sector size in bytes (usually 512) */
    uint64_t  num_sectors;     /* Total number of sectors */
    uint64_t  part_offset;     /* Partition start sector offset (0 for whole disk) */
    int       formatted;        /* 1 = has valid filesystem */
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

/* Get the list of all registered block devices */
BlockDev *BlockDev_GetList(void);

/* Read sectors from a block device */
int BlockDev_Read(BlockDev *dev, uint64_t sector, void *buffer, uint32_t num_sectors);

/* Write sectors to a block device */
int BlockDev_Write(BlockDev *dev, uint64_t sector, const void *buffer, uint32_t num_sectors);

/* Get device capacity in sectors */
uint64_t BlockDev_GetCapacity(BlockDev *dev);

/* Register a partition device (child of parent with offset).
 * Returns the registered BlockDev* on success, NULL on failure. */
BlockDev *BlockDev_RegisterPartition(BlockDev *parent, int part_index, uint32_t start_sector, uint32_t num_sectors, const char *display_name);

/* Unregister all partition devices of a parent */
void BlockDev_UnregisterPartitions(BlockDev *parent);

/* Detect if a partition has a valid filesystem (reads boot sector) */
int BlockDev_CheckFormatted(BlockDev *dev);

/* Read FAT32 volume label from boot sector into buf[max].
 * Returns 1 on success, 0 if not formatted / no label. */
int BlockDev_ReadVolLabel(BlockDev *dev, char *buf, int max);

/* Initialize block device layer */
void BlockDev_Init(void);

#endif /* UAOS_BLOCKDEV_H */
