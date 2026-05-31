/*
 * blockdev.c — UAOS Block Device Layer Implementation
 *
 * Provides a block device abstraction for storage devices like
 * VirtIO block devices. This layer handles sector-level I/O and
 * provides a unified interface for filesystems.
 */

#include "blockdev.h"
#include <stdio.h>
#include <string.h>

static void scpy(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* =========================================================================
 * Global State
 * ========================================================================= */

static BlockDev *g_blockdev_list = NULL;
static BlockDev g_blockdevs[MAX_BLOCKDEVS];
static uint32_t g_num_blockdevs = 0;

/* =========================================================================
 * Block Device Management
 * ========================================================================= */

int BlockDev_Register(BlockDev *dev)
{
    if (!dev || !dev->name || !dev->ops) {
        printf("[BLOCKDEV] Invalid device parameters\n");
        return -1;
    }

    if (g_num_blockdevs >= MAX_BLOCKDEVS) {
        printf("[BLOCKDEV] Maximum block devices reached\n");
        return -1;
    }

    /* Check for duplicate name */
    for (uint32_t i = 0; i < g_num_blockdevs; i++) {
        if (strcmp(g_blockdevs[i].name, dev->name) == 0) {
            printf("[BLOCKDEV] Device '%s' already registered\n", dev->name);
            return -1;
        }
    }

    /* Copy device to array */
    memcpy(&g_blockdevs[g_num_blockdevs], dev, sizeof(BlockDev));
    g_blockdevs[g_num_blockdevs].next = g_blockdev_list;
    g_blockdev_list = &g_blockdevs[g_num_blockdevs];
    g_num_blockdevs++;

    printf("[BLOCKDEV] Registered device '%s' (%llu sectors)\n", 
           dev->name, dev->num_sectors);
    return 0;
}

void BlockDev_Unregister(BlockDev *dev)
{
    if (!dev) return;

    /* Remove from list */
    BlockDev **pp = &g_blockdev_list;
    while (*pp) {
        if (*pp == dev) {
            *pp = dev->next;
            break;
        }
        pp = &(*pp)->next;
    }

    /* Clear device entry */
    for (uint32_t i = 0; i < g_num_blockdevs; i++) {
        if (&g_blockdevs[i] == dev) {
            memset(&g_blockdevs[i], 0, sizeof(BlockDev));
            break;
        }
    }

    printf("[BLOCKDEV] Unregistered device '%s'\n", dev->name);
}

BlockDev *BlockDev_Find(const char *name)
{
    if (!name) return NULL;

    BlockDev *dev = g_blockdev_list;
    while (dev) {
        if (strcmp(dev->name, name) == 0) {
            return dev;
        }
        dev = dev->next;
    }

    return NULL;
}

BlockDev *BlockDev_GetList(void)
{
    return g_blockdev_list;
}

/* =========================================================================
 * Block Device I/O Operations
 * ========================================================================= */

int BlockDev_Read(BlockDev *dev, uint64_t sector, void *buffer, uint32_t num_sectors)
{
    if (!dev || !dev->ops || !dev->ops->read) {
        printf("[BLOCKDEV] Invalid device or read operation\n");
        return -1;
    }

    if (sector + num_sectors > dev->num_sectors) {
        printf("[BLOCKDEV] Read beyond device capacity\n");
        return -1;
    }

    return dev->ops->read(sector + dev->part_offset, buffer, num_sectors);
}

int BlockDev_Write(BlockDev *dev, uint64_t sector, const void *buffer, uint32_t num_sectors)
{
    if (!dev) {
        printf("[BLOCKDEV] Write: dev is NULL\n");
        return -1;
    }
    if (!dev->ops) {
        printf("[BLOCKDEV] Write: ops is NULL\n");
        return -2;
    }
    if (!dev->ops->write) {
        printf("[BLOCKDEV] Write: ops->write is NULL\n");
        return -3;
    }

    if (sector + num_sectors > dev->num_sectors) {
        printf("[BLOCKDEV] Write beyond device capacity\n");
        return -4;
    }

    return dev->ops->write(sector + dev->part_offset, buffer, num_sectors);
}

uint64_t BlockDev_GetCapacity(BlockDev *dev)
{
    if (!dev) return 0;

    /* For partition devices, return the partition size, not parent disk size */
    if (dev->part_offset != 0) {
        return dev->num_sectors;
    }

    if (dev->ops && dev->ops->get_capacity) {
        return dev->ops->get_capacity();
    }

    return dev->num_sectors;
}

/* =========================================================================
 * Partition Device Registration
 * ========================================================================= */

int BlockDev_RegisterPartition(BlockDev *parent, int part_index, uint32_t start_sector, uint32_t num_sectors)
{
    if (!parent || part_index < 0 || part_index > 9) {
        printf("[BLOCKDEV] Invalid partition parameters\n");
        return -1;
    }

    if (g_num_blockdevs >= MAX_BLOCKDEVS) {
        printf("[BLOCKDEV] Maximum block devices reached\n");
        return -1;
    }

    /* Build partition name: parent name + digit */
    static char part_names[MAX_BLOCKDEVS][32];
    int pi = g_num_blockdevs;
    scpy(part_names[pi], parent->name, 32);
    int nl = 0;
    while (part_names[pi][nl]) nl++;
    part_names[pi][nl] = '0' + part_index;
    part_names[pi][nl + 1] = '\0';

    /* Check for duplicate */
    if (BlockDev_Find(part_names[pi]) != NULL) {
        printf("[BLOCKDEV] Partition '%s' already registered\n", part_names[pi]);
        return -1;
    }

    BlockDev part;
    memset(&part, 0, sizeof(part));
    part.name = part_names[pi];
    part.sector_size = parent->sector_size;
    part.num_sectors = num_sectors;
    part.part_offset = start_sector;
    part.private_data = parent->private_data;
    part.ops = parent->ops;

    return BlockDev_Register(&part);
}

void BlockDev_UnregisterPartitions(BlockDev *parent)
{
    if (!parent) return;

    /* Scan list and remove any device whose name starts with parent's name
     * followed by a digit */
    int parent_len = 0;
    while (parent->name[parent_len]) parent_len++;

    BlockDev *dev = g_blockdev_list;
    while (dev) {
        BlockDev *next = dev->next;
        if (dev != parent) {
            int match = 1;
            for (int i = 0; i < parent_len; i++) {
                if (dev->name[i] != parent->name[i]) { match = 0; break; }
            }
            if (match && dev->name[parent_len] >= '0' && dev->name[parent_len] <= '9') {
                BlockDev_Unregister(dev);
            }
        }
        dev = next;
    }
}

/* =========================================================================
 * Initialization
 * ========================================================================= */

void BlockDev_Init(void)
{
    memset(g_blockdevs, 0, sizeof(g_blockdevs));
    g_blockdev_list = NULL;
    g_num_blockdevs = 0;
    printf("[BLOCKDEV] Block device layer initialized\n");
}
