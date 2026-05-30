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

    return dev->ops->read(sector, buffer, num_sectors);
}

int BlockDev_Write(BlockDev *dev, uint64_t sector, const void *buffer, uint32_t num_sectors)
{
    if (!dev || !dev->ops || !dev->ops->write) {
        printf("[BLOCKDEV] Invalid device or write operation\n");
        return -1;
    }

    if (sector + num_sectors > dev->num_sectors) {
        printf("[BLOCKDEV] Write beyond device capacity\n");
        return -1;
    }

    return dev->ops->write(sector, buffer, num_sectors);
}

uint64_t BlockDev_GetCapacity(BlockDev *dev)
{
    if (!dev) return 0;

    if (dev->ops && dev->ops->get_capacity) {
        return dev->ops->get_capacity();
    }

    return dev->num_sectors;
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
