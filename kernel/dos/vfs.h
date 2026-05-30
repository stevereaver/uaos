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

/* -------------------------------------------------------------------------
 * File handle (returned by VFS_Open)
 * ------------------------------------------------------------------------- */
typedef struct {
    RamFsNode *node;    /* NULL = invalid / not open */
    uint32_t   pos;     /* current read/write position */
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

#endif
