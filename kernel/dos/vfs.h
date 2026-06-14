/* vfs.h — UAOS Virtual Filesystem Layer
 *
 * Thin dispatch layer over RAM volumes.
 * Paths use AmigaDOS format: "VOL:path/to/file"
 *
 * Supported volumes at boot:
 *   RAM:   — in-memory RAM disk (pre-populated with T, ENV, CLIPS)
 */

#ifndef UAOS_VFS_H
#define UAOS_VFS_H

#include <stdint.h>
#include "ramfs.h"
#include "dos/handler.h"

/* -------------------------------------------------------------------------
 * File handle (returned by VFS_Open)
 * ------------------------------------------------------------------------- */
typedef struct {
    RamFsNode *node;     /* NULL = invalid / not open */
    uint32_t   pos;      /* current read/write position */
    int        nil;      /* 1 = NIL: handle (discard writes, EOF on read) */
    uint32_t   handle_id;/* global HandleTable ID (0 = not tracked) */
} VfsFile;

/* -------------------------------------------------------------------------
 * Directory entry (filled by VFS_ReadDir)
 * ------------------------------------------------------------------------- */
typedef struct {
    char    name[RAMFS_MAX_NAME];
    uint8_t is_dir;     /* 1 = directory, 0 = file */
    uint32_t size;      /* file size (0 for dirs) */
} VfsDirEnt;

/* -------------------------------------------------------------------------
 * Open flags
 * ------------------------------------------------------------------------- */
#define VFS_READ    1
#define VFS_WRITE   2
#define VFS_CREATE  4   /* create if not exists */
#define VFS_TRUNC   8   /* truncate if exists */

/* -------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/* Initialise VFS + mount RAM: with standard dirs. Call once at boot. */
void VFS_Init(void);

/* Setup ROM fallback assigns immediately after the boot volume is identified.
 * Maps SYS: to the boot volume root and LIBS:/C:/S:/DEVS:/L: to standard
 * subdirectories.  This mirrors the Kickstart ROM pre-assign phase on Amiga.
 * Call after Workbench: volume is mounted. */
void VFS_SetupWorkbenchAssigns(void);

/* Mount a partition volume by name (e.g. "DH0", "WORK").
 * Creates an empty RAMFS backing volume so cd/dir work.
 * Returns 0 on success, -1 if mount table full or name too long. */
int VFS_MountPartition(const char *name);

/* Register an existing RAMFS volume with the VFS mount table.
 * Used when a volume is already populated (e.g. ISO9660 sub-volume).
 * Returns 0 on success, -1 if mount table full or name too long. */
int VFS_MountExistingVol(const char *name, RamFsVol *vol);

/* Return number of currently mounted volumes. */
int VFS_GetMountCount(void);

/* Get name of i-th mounted volume into dst[max].
 * Returns 1 on success, 0 if idx out of range. */
int VFS_GetMountName(int idx, char *dst, int max);

/* Open a file.  Returns 1 on success, 0 on failure. */
int  VFS_Open(VfsFile *fh, const char *path, int flags);

/* Close a file handle. */
void VFS_Close(VfsFile *fh);

/* Read up to len bytes into buf.  Returns bytes read. */
uint32_t VFS_Read(VfsFile *fh, uint8_t *buf, uint32_t len);

/* Write len bytes from buf.  Returns bytes written (or 0 on error). */
uint32_t VFS_Write(VfsFile *fh, const uint8_t *buf, uint32_t len);

/* Seek to absolute position. */
void VFS_Seek(VfsFile *fh, uint32_t pos);

/* Return file size, or 0 if not a file. */
uint32_t VFS_Size(VfsFile *fh);

/* Create a directory (and any missing parents on the path). */
int  VFS_MkDir(const char *path);

/* Delete a file or empty directory. */
int  VFS_Delete(const char *path);

/* Open a directory for reading.  Returns the first child node or NULL.
 * NOTE: returns NULL for empty directories — use VFS_ResolveDir to check
 * existence of a directory without caring about its contents. */
RamFsNode *VFS_OpenDir(const char *path);

/* Resolve a path to its directory node (returns the node itself, not children).
 * Returns NULL if path does not exist or is not a directory. */
RamFsNode *VFS_ResolveDir(const char *path);

/* Returns the volume root node for "RAM:" (for direct tree walking). */
RamFsNode *VFS_GetRoot(const char *vol_name);

/* Get attributes of a file/directory. Returns RAMFS_ATTR_* bit flags. */
uint8_t VFS_GetAttrs(const char *path);

/* Set attributes of a file/directory. Returns 0 on success, -1 on failure. */
int VFS_SetAttrs(const char *path, uint8_t attrs);

/* Get comment of a file/directory. Writes to dst[max], returns 0 on success. */
int VFS_GetComment(const char *path, char *dst, int max);

/* Set comment of a file/directory. Returns 0 on success, -1 on failure. */
int VFS_SetComment(const char *path, const char *comment);

/* Rename a volume. Returns 0 on success, -1 if not found. */
int VFS_RenameVol(const char *old_name, const char *new_name);

/* -------------------------------------------------------------------------
 * AmigaDOS Handler Support
 * ------------------------------------------------------------------------- */

/* Look up a mounted volume's packet handler by name (e.g. "RAM").
 * Returns NULL if the volume is not mounted. */
Handler *VFS_FindHandler(const char *vol_name);

/* Get the MsgPort of a mounted volume's handler.
 * Returns NULL if the volume is not mounted. */
MsgPort *VFS_GetHandlerPort(const char *vol_name);

/* Get the RamFsVol backing a mounted volume (for direct native access).
 * Returns NULL if the volume is not mounted. */
RamFsVol *VFS_FindVol(const char *vol_name);

/* -------------------------------------------------------------------------
 * AmigaDOS Assign Support
 * Assigns create logical names that map to physical paths.
 * ------------------------------------------------------------------------- */

/* Add an assign: maps assign_name -> target_path.
 *   add   : 1 = append to existing multi-assign, 0 = overwrite/create
 *   defer : 1 = skip target validation (DEFER), 0 = validate immediately
 * Returns 0 on success, -1 on failure (table full or invalid path). */
int VFS_AddAssign(const char *assign_name, const char *target_path,
                  int add, int defer);

/* Remove an assign by name. Returns 0 on success, -1 if not found. */
int VFS_RemoveAssign(const char *assign_name);

/* Resolve an assign name to its first target path.
 * Returns pointer to static buffer, or NULL if not an assign.
 * The returned string is valid until next call. */
const char *VFS_ResolveAssign(const char *assign_name);

/* Return the number of targets for a multi-assign. 0 = not found. */
int VFS_GetAssignTargetCount(const char *assign_name);

/* Return the i-th target of an assign (0-based). NULL if out of range. */
const char *VFS_GetAssignTarget(const char *assign_name, int idx);

/* List all assigns into buffer (one per line).
 * Multi-assign additional targets shown indented with '+ '.
 * Returns number of characters written. */
int VFS_ListAssigns(char *buf, int max);

/* Resolve a path that may contain assigns (e.g., "C:dir" -> "Workbench:C/dir").
 * Uses the first target for multi-assigns.
 * Writes resolved path to dst[max] and returns dst, or NULL if error.
 * The returned pointer is valid until next call. */
const char *VFS_ExpandAssigns(const char *path, char *dst, int max);

#endif
