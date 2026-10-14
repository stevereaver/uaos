/* adf_mount.c — ADF insert/eject/mount service for DF0:-DF3:
 *
 * AdfMount_Insert reads an ADF file through the VFS, loads it into the
 * emulated drive, attaches its block device and mounts the filesystem.
 * AdfMount_Eject unmounts the volume (refusing while files or locks are
 * open), writes a dirty image back to its source file, detaches the
 * block device and ejects the disk.
 *
 * Write-back policy (UAOS-161): a dirty ADF is saved to its source path
 * on eject unless the caller passes discard=1.  If the source could not
 * be opened for writing at insert time the drive is write-protected so
 * nothing is lost silently.  A failed write-back leaves the disk
 * loaded, dirty and unmounted so the eject can be retried or discarded.
 */
#include "adf_mount.h"
#include "vfs.h"
#include "blockdev.h"
#include "chipset/floppy.h"
#include "drivers/floppy_blk.h"
#include <stdint.h>
#include <string.h>

/* Per-unit bookkeeping for disks inserted via AdfMount_Insert. */
static struct {
    int  mounted;
    int  write_protected;
    char path[128];
    char volume[32];
} g_adf[FLOPPY_UNITS];

/* Staging buffer for the ADF file being inserted. */
static uint8_t g_adf_stage[FLOPPY_DISK_SIZE];

static int adf_str_eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Block device object for a unit, or NULL if not attached. */
static BlockDev *adf_bdev(int unit)
{
    char name[8] = "floppy0";
    name[6] = (char)('0' + unit);
    return BlockDev_Find(name);
}

/* Live mount index of this unit's blockdev, or -1 (also -1 when the
 * volume was dismounted by hand). */
static int adf_mount_index(int unit)
{
    BlockDev *bdev = adf_bdev(unit);
    return bdev ? VFS_FindMountByDev(bdev) : -1;
}

int AdfMount_Insert(int unit, const char *path)
{
    if (unit < 0 || unit >= FLOPPY_UNITS) return ADF_ERR_UNIT;
    if (!path || !*path) return ADF_ERR_OPEN;
    if (floppy_unit_loaded(unit)) return ADF_ERR_OCCUPIED;

    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ)) return ADF_ERR_OPEN;
    if (VFS_Size(&fh) != FLOPPY_DISK_SIZE) {
        VFS_Close(&fh);
        return ADF_ERR_SIZE;
    }
    uint32_t got = 0;
    while (got < FLOPPY_DISK_SIZE) {
        uint32_t n = VFS_Read(&fh, g_adf_stage + got,
                              (uint32_t)FLOPPY_DISK_SIZE - got);
        if (n == 0) break;
        got += n;
    }
    VFS_Close(&fh);
    if (got != FLOPPY_DISK_SIZE) return ADF_ERR_OPEN;

    if (!(g_adf_stage[0] == 'D' && g_adf_stage[1] == 'O' &&
          g_adf_stage[2] == 'S'))
        return ADF_ERR_NDOS;

    /* Probe writability before the image goes live: an ADF on a
     * read-only source (CD, protected file) inserts write-protected so
     * FS writes fail visibly instead of being lost on eject.  A
     * write-open without writes has no side effect on either handler —
     * FFS FINDUPDATE only resolves the file key (ffs_handler.c) and
     * FAT32 FINDUPDATE uses FAT32_Open (fat_handler.c). */
    int writable = 0;
    {
        VfsFile pf;
        if (VFS_Open(&pf, path, VFS_WRITE)) {
            VFS_Close(&pf);
            writable = 1;
        }
    }

    floppy_load_adf(unit, g_adf_stage, FLOPPY_DISK_SIZE);
    floppy_set_write_protect(unit, writable ? 0 : 1);
    if (FloppyBlk_Attach(unit) != 0) {
        floppy_eject(unit);
        return ADF_ERR_MOUNT;
    }

    char dname[5] = "DF0:";
    dname[2] = (char)('0' + unit);
    int mret = VFS_MountPartition(dname);
    int midx = adf_mount_index(unit);
    if (mret != 0 || midx < 0) {
        /* VFS_MountPartition returns 0 without mounting our bdev when a
         * same-named volume is already mounted -> name clash. */
        int err = (mret == 0) ? ADF_ERR_NAMECLASH : ADF_ERR_MOUNT;
        FloppyBlk_Detach(unit);
        floppy_eject(unit);
        return err;
    }

    int i = 0;
    while (path[i] && i < (int)sizeof(g_adf[unit].path) - 1) {
        g_adf[unit].path[i] = path[i];
        i++;
    }
    g_adf[unit].path[i] = '\0';
    VFS_GetMountName(midx, g_adf[unit].volume, sizeof(g_adf[unit].volume));
    g_adf[unit].write_protected = writable ? 0 : 1;
    g_adf[unit].mounted = 1;
    return ADF_OK;
}

/* Write the in-memory ADF back to its source file.  The file is exactly
 * FLOPPY_DISK_SIZE bytes, so a sequential VFS_Write from offset 0 is an
 * in-place overwrite (VFS_WRITE without VFS_TRUNC -> ACTION_FINDUPDATE,
 * which does not truncate on either handler).  Returns 0 on success. */
static int adf_writeback(int unit)
{
    const uint8_t *img = floppy_adf_data(unit);
    if (!img) return -1;

    VfsFile fh;
    if (!VFS_Open(&fh, g_adf[unit].path, VFS_WRITE)) return -1;
    uint32_t done = 0;
    while (done < FLOPPY_DISK_SIZE) {
        uint32_t n = VFS_Write(&fh, img + done,
                               (uint32_t)FLOPPY_DISK_SIZE - done);
        if (n == 0) break;
        done += n;
    }
    uint32_t sz = VFS_Size(&fh);
    VFS_Close(&fh);
    if (done != FLOPPY_DISK_SIZE || sz != FLOPPY_DISK_SIZE) return -1;
    return 0;
}

int AdfMount_Eject(int unit, int discard)
{
    if (unit < 0 || unit >= FLOPPY_UNITS) return ADF_ERR_UNIT;
    if (!floppy_unit_loaded(unit)) return ADF_ERR_EMPTY;

    /* Unmount first if this unit's blockdev still has a mount — this is
     * what flushes FS state into the floppy image before write-back. */
    int midx = adf_mount_index(unit);
    if (midx >= 0) {
        char unit_name[32];
        if (!VFS_GetMountInfo(midx, unit_name, sizeof(unit_name), NULL, 0))
            return ADF_ERR_MOUNT;
        int r = VFS_Unmount(unit_name);
        if (r == VFS_UNMOUNT_BUSY) return ADF_ERR_VOLBUSY;
        if (r != VFS_UNMOUNT_OK)  return ADF_ERR_MOUNT;
    }

    /* Write a dirty image back to its source.  Disks with no recorded
     * path (the selftest disk) or discard=1 skip this.  On failure the
     * disk stays loaded, dirty and unmounted — a retry saves again and
     * `adf eject <u> discard` drops the changes. */
    if (floppy_is_dirty(unit) && g_adf[unit].path[0] && !discard) {
        if (adf_writeback(unit) != 0)
            return ADF_ERR_WRITEBACK;
        floppy_clear_dirty(unit);
    }

    FloppyBlk_Detach(unit);
    floppy_eject(unit);
    memset(&g_adf[unit], 0, sizeof(g_adf[unit]));
    return ADF_OK;
}

int AdfMount_UnitInfo(int unit, AdfUnitInfo *out)
{
    if (unit < 0 || unit >= FLOPPY_UNITS || !out) return ADF_ERR_UNIT;
    memset(out, 0, sizeof(*out));
    out->loaded = floppy_unit_loaded(unit);
    out->dirty  = floppy_is_dirty(unit);
    out->write_protected = g_adf[unit].write_protected;
    int midx = adf_mount_index(unit);
    out->mounted = (midx >= 0);
    if (midx >= 0)
        VFS_GetMountName(midx, out->volume, sizeof(out->volume));
    else if (g_adf[unit].volume[0])
        memcpy(out->volume, g_adf[unit].volume, sizeof(out->volume));
    memcpy(out->path, g_adf[unit].path, sizeof(out->path));
    return ADF_OK;
}

int AdfMount_UnitForVolume(const char *vol)
{
    if (!vol || !*vol) return -1;
    char name[32];
    int i = 0;
    while (vol[i] && vol[i] != ':' && i < (int)sizeof(name) - 1) {
        name[i] = vol[i];
        i++;
    }
    name[i] = '\0';
    for (int u = 0; u < FLOPPY_UNITS; u++) {
        if (adf_mount_index(u) < 0) continue;
        char cur[32] = {0};
        VFS_GetMountName(adf_mount_index(u), cur, sizeof(cur));
        if (adf_str_eq_ci(cur, name)) return u;
    }
    return -1;
}

const char *AdfMount_ErrorString(int err)
{
    switch (err) {
    case ADF_OK:            return "OK";
    case ADF_ERR_UNIT:      return "Invalid unit";
    case ADF_ERR_OCCUPIED:  return "Disk already in drive";
    case ADF_ERR_OPEN:      return "Cannot open file";
    case ADF_ERR_SIZE:      return "Not an ADF image (bad size)";
    case ADF_ERR_NDOS:      return "Not a DOS disk";
    case ADF_ERR_MOUNT:     return "Mount failed";
    case ADF_ERR_NAMECLASH: return "Volume name already in use";
    case ADF_ERR_EMPTY:     return "No disk in drive";
    case ADF_ERR_VOLBUSY:   return "Volume is in use";
    case ADF_ERR_WRITEBACK: return "Could not save changes to ADF file";
    default:                return "Unknown error";
    }
}
