/* iso9660.h — UAOS ISO 9660 Level 2 Filesystem Reader
 *
 * Simplified ISO 9660 reader for CD-ROM boot volumes.
 * Supports Level 2 filenames (31 chars), Rock Ridge NM (long names),
 * and basic directory traversal.
 */

#ifndef UAOS_ISO9660_H
#define UAOS_ISO9660_H

#include "blockdev.h"

/* Mount an ISO 9660 CD by reading its contents into a RAMFS volume.
 * bdev: ATAPI CD-ROM block device (2048-byte sectors)
 * vol_name: name for the mounted volume (e.g. "UAOSCD")
 * Returns 0 on success, negative on error. */
int ISO9660_MountCD(BlockDev *bdev, const char *vol_name);

#endif /* UAOS_ISO9660_H */
