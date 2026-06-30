/* floppy_blk.c */
#include "drivers/floppy_blk.h"
#include "dos/blockdev.h"
#include "chipset/floppy.h"
#include <string.h>

typedef struct {
    int loaded;
} FloppyBlkPriv;

static FloppyBlkPriv g_floppy_blk_priv;
static BlockDevOps g_floppy_blk_ops;
static BlockDev g_floppy_blk_dev;

static int floppy_blk_read(BlockDev *bdev, uint64_t sector, void *buffer, uint32_t num_sectors)
{
    (void)bdev;
    if (!g_floppy_blk_priv.loaded) return -1;
    uint64_t total = (uint64_t)FLOPPY_TRACKS * FLOPPY_HEADS * FLOPPY_SECTORS;
    if (sector + num_sectors > total) return -1;

    uint8_t *out = (uint8_t *)buffer;
    for (uint32_t i = 0; i < num_sectors; i++) {
        uint64_t s = sector + i;
        int track = (int)(s / FLOPPY_SECTORS);
        int sec = (int)(s % FLOPPY_SECTORS);
        if (!floppy_read_sector(track, sec, out + i * FLOPPY_SECTOR_SIZE)) return -1;
    }
    return 0;
}

static int floppy_blk_write(BlockDev *bdev, uint64_t sector, const void *buffer, uint32_t num_sectors)
{
    (void)bdev;
    if (!g_floppy_blk_priv.loaded) return -1;
    uint64_t total = (uint64_t)FLOPPY_TRACKS * FLOPPY_HEADS * FLOPPY_SECTORS;
    if (sector + num_sectors > total) return -1;

    const uint8_t *in = (const uint8_t *)buffer;
    for (uint32_t i = 0; i < num_sectors; i++) {
        uint64_t s = sector + i;
        int track = (int)(s / FLOPPY_SECTORS);
        int sec = (int)(s % FLOPPY_SECTORS);
        if (!floppy_write_sector(track, sec, in + i * FLOPPY_SECTOR_SIZE)) return -1;
    }
    return 0;
}

static uint64_t floppy_blk_capacity(BlockDev *bdev)
{
    (void)bdev;
    return (uint64_t)FLOPPY_TRACKS * FLOPPY_HEADS * FLOPPY_SECTORS;
}

int FloppyBlockDev_Init(void)
{
    if (!g_floppy.adf_loaded) return -1;
    g_floppy_blk_priv.loaded = 1;

    g_floppy_blk_ops.read = floppy_blk_read;
    g_floppy_blk_ops.write = floppy_blk_write;
    g_floppy_blk_ops.get_capacity = floppy_blk_capacity;

    static char name[] = "floppy0";
    static char dname[] = "DF0:";

    g_floppy_blk_dev.name = name;
    g_floppy_blk_dev.display_name = dname;
    g_floppy_blk_dev.sector_size = FLOPPY_SECTOR_SIZE;
    g_floppy_blk_dev.num_sectors = floppy_blk_capacity(NULL);
    g_floppy_blk_dev.part_offset = 0;
    g_floppy_blk_dev.formatted = 0;
    g_floppy_blk_dev.private_data = &g_floppy_blk_priv;
    g_floppy_blk_dev.ops = &g_floppy_blk_ops;
    g_floppy_blk_dev.next = NULL;

    return BlockDev_Register(&g_floppy_blk_dev);
}

/* Verify that the floppy block-device write path round-trips a sector. */
int floppy_block_device_write_test(void)
{
    BlockDev *dev = BlockDev_Find("floppy0");
    if (!dev) return 0;

    uint8_t buf[512];
    for (int i = 0; i < 512; i++) buf[i] = (uint8_t)(0xA5 ^ i);

    if (BlockDev_Write(dev, 2, buf, 1) != 0) return 0;

    uint8_t readback[512];
    if (BlockDev_Read(dev, 2, readback, 1) != 0) return 0;

    return memcmp(buf, readback, 512) == 0;
}
