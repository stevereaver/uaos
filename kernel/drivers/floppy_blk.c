/* floppy_blk.c — per-drive floppy block devices (floppy0..3 / DF0:..DF3:) */
#include "drivers/floppy_blk.h"
#include "dos/blockdev.h"
#include "chipset/floppy.h"
#include <string.h>

typedef struct {
    int  unit;
    int  attached;
    char name[8];   /* "floppyN" */
    char dname[6];  /* "DFN:" */
} FloppyBlkPriv;

static FloppyBlkPriv g_floppy_blk_priv[FLOPPY_UNITS];
static BlockDevOps g_floppy_blk_ops;

static int floppy_blk_read(BlockDev *bdev, uint64_t sector, void *buffer, uint32_t num_sectors)
{
    FloppyBlkPriv *p = (FloppyBlkPriv *)bdev->private_data;
    if (!p || !floppy_unit_loaded(p->unit)) return -1;
    uint64_t total = (uint64_t)FLOPPY_TRACKS * FLOPPY_HEADS * FLOPPY_SECTORS;
    if (sector + num_sectors > total) return -1;

    uint8_t *out = (uint8_t *)buffer;
    for (uint32_t i = 0; i < num_sectors; i++) {
        uint64_t s = sector + i;
        int track = (int)(s / FLOPPY_SECTORS);
        int sec = (int)(s % FLOPPY_SECTORS);
        if (!floppy_read_sector(p->unit, track, sec, out + i * FLOPPY_SECTOR_SIZE)) return -1;
    }
    return 0;
}

static int floppy_blk_write(BlockDev *bdev, uint64_t sector, const void *buffer, uint32_t num_sectors)
{
    FloppyBlkPriv *p = (FloppyBlkPriv *)bdev->private_data;
    if (!p || !floppy_unit_loaded(p->unit)) return -1;
    uint64_t total = (uint64_t)FLOPPY_TRACKS * FLOPPY_HEADS * FLOPPY_SECTORS;
    if (sector + num_sectors > total) return -1;

    const uint8_t *in = (const uint8_t *)buffer;
    for (uint32_t i = 0; i < num_sectors; i++) {
        uint64_t s = sector + i;
        int track = (int)(s / FLOPPY_SECTORS);
        int sec = (int)(s % FLOPPY_SECTORS);
        if (!floppy_write_sector(p->unit, track, sec, in + i * FLOPPY_SECTOR_SIZE)) return -1;
    }
    return 0;
}

static uint64_t floppy_blk_capacity(BlockDev *bdev)
{
    (void)bdev;
    return (uint64_t)FLOPPY_TRACKS * FLOPPY_HEADS * FLOPPY_SECTORS;
}

/* Register a block device for a drive that currently has media loaded. */
int FloppyBlk_Attach(int unit)
{
    if (unit < 0 || unit >= FLOPPY_UNITS) return -1;
    FloppyBlkPriv *p = &g_floppy_blk_priv[unit];
    if (p->attached) return 0;
    if (!floppy_unit_loaded(unit)) return -1;

    g_floppy_blk_ops.read = floppy_blk_read;
    g_floppy_blk_ops.write = floppy_blk_write;
    g_floppy_blk_ops.get_capacity = floppy_blk_capacity;

    p->unit = unit;
    p->name[0] = 'f'; p->name[1] = 'l'; p->name[2] = 'o'; p->name[3] = 'p';
    p->name[4] = 'p'; p->name[5] = 'y'; p->name[6] = (char)('0' + unit);
    p->name[7] = '\0';
    p->dname[0] = 'D'; p->dname[1] = 'F'; p->dname[2] = (char)('0' + unit);
    p->dname[3] = ':'; p->dname[4] = '\0';

    BlockDev dev;
    memset(&dev, 0, sizeof(dev));
    dev.name = p->name;
    dev.display_name = p->dname;
    dev.sector_size = FLOPPY_SECTOR_SIZE;
    dev.num_sectors = floppy_blk_capacity(NULL);
    dev.part_offset = 0;
    dev.formatted = 0;
    dev.private_data = p;
    dev.ops = &g_floppy_blk_ops;
    dev.next = NULL;

    if (BlockDev_Register(&dev) != 0) return -1;
    p->attached = 1;
    return 0;
}

/* Unregister a drive's block device.  The caller is responsible for
 * ensuring no volume is mounted on it. */
void FloppyBlk_Detach(int unit)
{
    if (unit < 0 || unit >= FLOPPY_UNITS) return;
    FloppyBlkPriv *p = &g_floppy_blk_priv[unit];
    if (!p->attached) return;
    BlockDev *dev = BlockDev_Find(p->name);
    if (dev) BlockDev_Unregister(dev);
    p->attached = 0;
}

/* Attach every drive that currently has a disk inserted.
 * Returns the number of drives registered. */
int FloppyBlockDev_Init(void)
{
    int n = 0;
    for (int u = 0; u < FLOPPY_UNITS; u++) {
        if (FloppyBlk_Attach(u) == 0) n++;
    }
    return n;
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

/* Verify per-unit data isolation plus hot attach/detach across DF0:-DF3:.
 * Unit 0 must be loaded and attached; units 1-3 are loaded, exercised and
 * always cleaned up afterwards. */
int floppy_multi_unit_test(void)
{
    if (!floppy_unit_loaded(0) || !g_floppy_blk_priv[0].attached) return 0;

    uint8_t saved[FLOPPY_SECTOR_SIZE];
    uint8_t pat[FLOPPY_SECTOR_SIZE];
    uint8_t buf[FLOPPY_SECTOR_SIZE];
    int ok = floppy_read_sector(0, 0, 5, saved);

    /* Load + attach units 1-3 and give each a distinct sector-5 pattern. */
    for (int u = 1; u < FLOPPY_UNITS && ok; u++) {
        floppy_make_test_adf(u);
        if (FloppyBlk_Attach(u) != 0) { ok = 0; break; }
        BlockDev *dev = BlockDev_Find(g_floppy_blk_priv[u].name);
        for (int i = 0; i < FLOPPY_SECTOR_SIZE; i++)
            pat[i] = (uint8_t)((u * 0x11) ^ i);
        if (!dev || BlockDev_Write(dev, 5, pat, 1) != 0) { ok = 0; break; }
    }

    /* Each unit reads back its own pattern; unit 0 is unchanged. */
    for (int u = 1; u < FLOPPY_UNITS && ok; u++) {
        BlockDev *dev = BlockDev_Find(g_floppy_blk_priv[u].name);
        if (!dev || BlockDev_Read(dev, 5, buf, 1) != 0) { ok = 0; break; }
        for (int i = 0; i < FLOPPY_SECTOR_SIZE; i++) {
            if (buf[i] != (uint8_t)((u * 0x11) ^ i)) { ok = 0; break; }
        }
    }
    if (ok && (!floppy_read_sector(0, 0, 5, buf) ||
               memcmp(saved, buf, FLOPPY_SECTOR_SIZE) != 0)) ok = 0;

    /* Detach + eject unit 2: its device vanishes and reads fail, while
     * units 1 and 3 keep their data. */
    if (ok) {
        FloppyBlk_Detach(2);
        floppy_eject(2);
        if (BlockDev_Find("floppy2") != NULL) ok = 0;
        if (ok && floppy_read_sector(2, 0, 5, buf) != 0) ok = 0;
        for (int u = 1; u < FLOPPY_UNITS && ok; u += 2) {
            BlockDev *dev = BlockDev_Find(g_floppy_blk_priv[u].name);
            if (!dev || BlockDev_Read(dev, 5, buf, 1) != 0) { ok = 0; break; }
            for (int i = 0; i < FLOPPY_SECTOR_SIZE; i++) {
                if (buf[i] != (uint8_t)((u * 0x11) ^ i)) { ok = 0; break; }
            }
        }
    }

    /* Always clean up the test units. */
    for (int u = 1; u < FLOPPY_UNITS; u++) {
        FloppyBlk_Detach(u);
        floppy_eject(u);
    }
    if (ok) {
        if (BlockDev_Find("floppy1") || BlockDev_Find("floppy2") ||
            BlockDev_Find("floppy3")) ok = 0;
        if (!BlockDev_Find("floppy0")) ok = 0;
    }
    return ok;
}

/* -------------------------------------------------------------------------
 * C:diskdiag — floppy block layer stage dump (UAOS-204)
 * ------------------------------------------------------------------------- */
#include "../dbg/diag.h"

void FloppyBlk_DiagDump(void *ctx, void (*emit)(void *, const char *))
{
    for (int u = 0; u < FLOPPY_UNITS; u++) {
        FloppyBlkPriv *p = &g_floppy_blk_priv[u];
        DiagLine l;
        dl_reset(&l);
        dl_add(&l, " floppy "); dl_dec(&l, (uint64_t)u);
        dl_add(&l, ": adf_loaded="); dl_dec(&l, (uint64_t)floppy_unit_loaded(u));
        dl_add(&l, " blk_attached="); dl_dec(&l, (uint64_t)p->attached);
        dl_add(&l, " bdev='");
        dl_add(&l, p->attached ? p->name : "-");
        dl_add(&l, "'");
        dl_emit(&l, ctx, emit);
    }
}
