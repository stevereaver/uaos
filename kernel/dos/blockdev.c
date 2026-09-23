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

/* Serialize all block-device I/O.  The VirtIO drivers use a single set of
 * global request/response/data buffers and a single in-flight descriptor,
 * so concurrent read/write from different tasks (e.g. desktop polling + shell
 * format/makedir) collides and corrupts the single in-flight transaction.
 * cli/sti is a sufficient global lock on the single x86-64 CPU this kernel
 * runs on. */
static inline void bd_cli(void) { __asm__ volatile("cli"); }
static inline void bd_sti(void) { __asm__ volatile("sti"); }

int BlockDev_Read(BlockDev *dev, uint64_t sector, void *buffer, uint32_t num_sectors)
{
    bd_cli();
    int rc = -1;

    if (!dev || !dev->ops || !dev->ops->read) {
        printf("[BLOCKDEV] Invalid device or read operation\n");
    } else if (sector + num_sectors > dev->num_sectors) {
        printf("[BLOCKDEV] Read beyond device capacity\n");
    } else {
        rc = dev->ops->read(dev, sector + dev->part_offset, buffer, num_sectors);
    }

    bd_sti();
    return rc;
}

int BlockDev_Write(BlockDev *dev, uint64_t sector, const void *buffer, uint32_t num_sectors)
{
    bd_cli();
    int rc = -1;

    if (!dev) {
        printf("[BLOCKDEV] Write: dev is NULL\n");
    } else if (!dev->ops) {
        printf("[BLOCKDEV] Write: ops is NULL\n");
        rc = -2;
    } else if (!dev->ops->write) {
        printf("[BLOCKDEV] Write: ops->write is NULL\n");
        rc = -3;
    } else if (sector + num_sectors > dev->num_sectors) {
        printf("[BLOCKDEV] Write beyond device capacity\n");
        rc = -4;
    } else {
        rc = dev->ops->write(dev, sector + dev->part_offset, buffer, num_sectors);
    }

    bd_sti();
    return rc;
}

uint64_t BlockDev_GetCapacity(BlockDev *dev)
{
    if (!dev) return 0;

    /* For partition devices, return the partition size, not parent disk size */
    if (dev->part_offset != 0) {
        return dev->num_sectors;
    }

    if (dev->ops && dev->ops->get_capacity) {
        return dev->ops->get_capacity(dev);
    }

    return dev->num_sectors;
}

/* =========================================================================
 * Partition Device Registration
 * ========================================================================= */

BlockDev *BlockDev_RegisterPartition(BlockDev *parent, int part_index, uint32_t start_sector, uint32_t num_sectors, const char *display_name)
{
    if (!parent || part_index < 0 || part_index > 9) {
        printf("[BLOCKDEV] Invalid partition parameters\n");
        return NULL;
    }

    if (g_num_blockdevs >= MAX_BLOCKDEVS) {
        printf("[BLOCKDEV] Maximum block devices reached\n");
        return NULL;
    }

    /* Build partition name: parent name + digit */
    static char part_names[MAX_BLOCKDEVS][32];
    static char disp_names[MAX_BLOCKDEVS][16];
    int pi = g_num_blockdevs;
    scpy(part_names[pi], parent->name, 32);
    int nl = 0;
    while (part_names[pi][nl]) nl++;
    part_names[pi][nl] = '0' + part_index;
    part_names[pi][nl + 1] = '\0';

    /* Check for duplicate */
    if (BlockDev_Find(part_names[pi]) != NULL) {
        printf("[BLOCKDEV] Partition '%s' already registered\n", part_names[pi]);
        return NULL;
    }

    /* Store display name */
    if (display_name && display_name[0]) {
        scpy(disp_names[pi], display_name, 16);
    } else {
        /* Default: DH0:, DH1:, etc. */
        disp_names[pi][0] = 'D'; disp_names[pi][1] = 'H';
        disp_names[pi][2] = '0' + (part_index - 1);
        disp_names[pi][3] = ':'; disp_names[pi][4] = '\0';
    }

    BlockDev part;
    memset(&part, 0, sizeof(part));
    part.name = part_names[pi];
    part.display_name = disp_names[pi];
    part.sector_size = parent->sector_size;
    part.num_sectors = num_sectors;
    part.part_offset = start_sector;
    part.private_data = parent->private_data;
    part.ops = parent->ops;

    if (BlockDev_Register(&part) != 0)
        return NULL;

    return BlockDev_Find(part_names[pi]);
}

/* 4K-aligned buffer for format-probe reads — must be DMA-safe for virtio-blk */
static uint8_t blockdev_boot_sector[512] __attribute__((aligned(4096)));

int BlockDev_CheckFormatted(BlockDev *dev)
{
    if (!dev) return 0;

    memset(blockdev_boot_sector, 0, 512);

    if (BlockDev_Read(dev, 0, blockdev_boot_sector, 1) != 0) {
        return 0;
    }

    /* Check boot signature 0x55AA at offset 510 */
    uint16_t sig = blockdev_boot_sector[510] | (blockdev_boot_sector[511] << 8);
    if (sig != 0xAA55) {
        return 0;
    }

    /* Also check for FAT32 signature in the BPB */
    /* bytes_per_sec should be 512, and sec_per_clus should be power of 2 */
    uint16_t bps = blockdev_boot_sector[11] | (blockdev_boot_sector[12] << 8);
    uint8_t spc = blockdev_boot_sector[13];
    if (bps != 512 || spc == 0 || (spc & (spc - 1)) != 0) {
        return 0;  /* Not a valid FAT BPB */
    }

    return 1;
}

int BlockDev_ReadVolLabel(BlockDev *dev, char *buf, int max)
{
    if (!dev || max < 2) return 0;

    /* Use the DMA-safe static buffer (blockdev_boot_sector) instead of a
     * stack buffer — the VirtIO driver requires DMA-accessible buffers. */
    memset(blockdev_boot_sector, 0, 512);

    if (BlockDev_Read(dev, 0, blockdev_boot_sector, 1) != 0)
        return 0;

    if (blockdev_boot_sector[510] != 0x55 || blockdev_boot_sector[511] != 0xAA)
        return 0;

    /* FAT32 volume label at offset 71, 11 bytes, space-padded.
     * Strip trailing spaces (don't stop at the first space, since
     * labels like "MY DISK" contain internal spaces). */
    int last_non_space = -1;
    for (int i = 0; i < 11; i++) {
        uint8_t c = blockdev_boot_sector[71 + i];
        if (c != ' ') last_non_space = i;
    }
    int n = 0;
    for (int i = 0; i <= last_non_space && n < max - 1; i++) {
        buf[n++] = blockdev_boot_sector[71 + i];
    }
    buf[n] = '\0';
    return n > 0 ? 1 : 0;
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
