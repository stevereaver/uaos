/* adf_mount.h — ADF insert/eject/mount service for DF0:-DF3:
 *
 * Bridges host-side ADF files (read through the VFS) and the emulated
 * floppy drives: insert loads the image into a drive, attaches its
 * block device and mounts the filesystem; eject unmounts, writes a
 * dirty image back to its source file (unless discard is requested or
 * the disk came from a read-only/unwritable source), detaches and
 * removes the disk.
 */
#ifndef UAOS_ADF_MOUNT_H
#define UAOS_ADF_MOUNT_H

#define ADF_OK              0
#define ADF_ERR_UNIT       -1   /* unit out of range */
#define ADF_ERR_OCCUPIED   -2   /* disk already in drive */
#define ADF_ERR_OPEN       -3   /* cannot open/read the file */
#define ADF_ERR_SIZE       -4   /* not 901120 bytes */
#define ADF_ERR_NDOS       -5   /* no DOS\x bootblock */
#define ADF_ERR_MOUNT      -6   /* blockdev/FS mount failed */
#define ADF_ERR_NAMECLASH  -7   /* a mounted volume already uses this disk's name */
#define ADF_ERR_EMPTY      -8   /* no disk in drive */
#define ADF_ERR_VOLBUSY    -9   /* files/locks open on the volume */
#define ADF_ERR_WRITEBACK  -10  /* could not save changes to the ADF file */

typedef struct {
    int  loaded;            /* disk present in the drive */
    int  mounted;           /* volume currently mounted */
    int  dirty;             /* ADF modified since insert */
    int  write_protected;   /* source file not writable at insert */
    char path[128];         /* source file (empty if not via AdfMount_Insert) */
    char volume[32];        /* mounted volume name */
} AdfUnitInfo;

int  AdfMount_Insert(int unit, const char *path);
int  AdfMount_Eject(int unit, int discard);
int  AdfMount_UnitInfo(int unit, AdfUnitInfo *out);   /* 0 ok, ADF_ERR_UNIT */
int  AdfMount_UnitForVolume(const char *vol);         /* "Name" or "Name:", -1 if none */
const char *AdfMount_ErrorString(int err);

#endif /* UAOS_ADF_MOUNT_H */
