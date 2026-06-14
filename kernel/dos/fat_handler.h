/* fat_handler.h — AmigaDOS packet handler for FAT32 block devices
 *
 * Wraps the existing FAT32 driver (fat32.c) in the Handler/DoPkt model.
 * Proves the architecture scales beyond RAMFS.
 */

#ifndef UAOS_FAT_HANDLER_H
#define UAOS_FAT_HANDLER_H

#include "dos/handler.h"
#include "dos/fat32.h"

/* Attach a FAT32 filesystem (already mounted via FAT32_Mount) to the
 * Handler system and register it under 'name' (e.g. "FAT").
 * Returns the handler on success, NULL on failure.
 * Use &handler->port to get the MsgPort*. */
Handler *FatHandler_Create(const char *name, Fat32FS *fs);

#endif
