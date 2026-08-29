/* crossdos_handler.h — CrossDOS FAT12/16 filesystem handler
 *
 * CrossDOS allows reading PC-format (MS-DOS FAT12/FAT16) floppy disks
 * from AmigaDOS.  This handler wraps a block device with FAT12/16
 * filesystem support and exposes it as an AmigaDOS volume.
 */

#ifndef UAOS_CROSSDOS_HANDLER_H
#define UAOS_CROSSDOS_HANDLER_H

#include "dos/handler.h"
#include "dos/blockdev.h"

/* Create a CrossDOS handler for the given block device.
 * Probes the boot sector to determine FAT12 vs FAT16.
 * Returns NULL if the device is not a valid FAT12/16 volume. */
Handler *CrossDOSHandler_Create(const char *name, BlockDev *bdev);

/* Probe a block device for FAT12/FAT16 filesystem.
 * Returns 1 if valid FAT12/16, 0 otherwise. */
int CrossDOS_Probe(BlockDev *bdev);

#endif
