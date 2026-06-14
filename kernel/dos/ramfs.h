/* ramfs.h — UAOS In-Memory RAM Filesystem
 *
 * A simple tree-structured filesystem stored entirely in BSS.
 * Provides AmigaDOS-style semantics: volumes, directories, files.
 * No block device, no checksums — just a node tree with byte arrays.
 */

#ifndef UAOS_RAMFS_H
#define UAOS_RAMFS_H

#include <stdint.h>
#include "blockdev.h"

/* -------------------------------------------------------------------------
 * Limits
 * ------------------------------------------------------------------------- */
#define RAMFS_MAX_NODES     1024    /* total nodes across all RAM volumes     */
#define RAMFS_MAX_NAME      32      /* max filename length (incl. NUL)        */
#define RAMFS_MAX_FILESIZE  524288  /* max bytes per file (512 KB)            */
#define RAMFS_FILE_POOL     (RAMFS_MAX_NODES * RAMFS_MAX_FILESIZE / 4)
                                    /* ~4 MB data pool in BSS                 */

/* -------------------------------------------------------------------------
 * Node types
 * ------------------------------------------------------------------------- */
#define RAMFS_TYPE_FREE  0
#define RAMFS_TYPE_DIR   1
#define RAMFS_TYPE_FILE  2

/* -------------------------------------------------------------------------
 * Node attributes (bit flags)
 * ------------------------------------------------------------------------- */
#define RAMFS_ATTR_READONLY  0x01  /* Read-only flag */
#define RAMFS_ATTR_HIDDEN    0x02  /* Hidden flag */

/* -------------------------------------------------------------------------
 * Node structure
 * Each node is either a directory or a file.
 * Directories contain a linked list of child nodes.
 * Files point into a shared data pool.
 * ------------------------------------------------------------------------- */
typedef struct RamFsNode {
    uint8_t  type;                  /* RAMFS_TYPE_*                           */
    uint8_t  attrs;                 /* RAMFS_ATTR_* bit flags                  */
    uint16_t protection;            /* AmigaDOS FIBF_* bits (inverted logic)   */
    char     name[RAMFS_MAX_NAME];  /* entry name (no path separator)         */
    struct RamFsNode *parent;       /* NULL for volume root                   */
    struct RamFsNode *first_child;  /* first child (dirs only)                */
    struct RamFsNode *next_sibling; /* linked list of siblings                */
    /* File metadata */
    char     comment[64];           /* file comment (filenote)                */
    /* File data */
    uint8_t *data;                  /* pointer into g_ramfs_pool              */
    uint32_t size;                  /* current file size in bytes             */
    uint32_t alloc;                 /* allocated bytes in pool                */
    /* External (proxy) file — data lives on block device, not in RAM         */
    BlockDev *ext_bdev;             /* NULL = normal RAM file                  */
    uint32_t  ext_lba;              /* starting LBA on block device            */
    uint32_t  ext_blksz;            /* block size (e.g. 2048 for ISO)          */
} RamFsNode;

/* -------------------------------------------------------------------------
 * Volume handle (one per mounted RAM volume)
 * ------------------------------------------------------------------------- */
typedef struct {
    char      name[16];   /* volume name, e.g. "RAM"                         */
    RamFsNode *root;      /* root directory node                             */
    int        valid;     /* non-zero if mounted                             */
} RamFsVol;

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/* Initialise the node pool.  Call once at boot. */
void RamFS_Init(void);

/* Create and mount a new RAM volume.  Returns pointer or NULL on failure. */
RamFsVol *RamFS_MountVol(const char *name);

/* Resolve an absolute path (e.g. "RAM:T/foo.txt") to a node.
 * Returns NULL if not found.  vol must be the matching mounted volume. */
RamFsNode *RamFS_Resolve(RamFsVol *vol, const char *path);

/* Create a directory at path (parent must exist). */
RamFsNode *RamFS_MkDir(RamFsVol *vol, const char *path);

/* Create or truncate a file at path (parent must exist). */
RamFsNode *RamFS_Create(RamFsVol *vol, const char *path);

/* Write data to a file (overwrites from offset 0, sets size). */
int RamFS_Write(RamFsNode *node, const uint8_t *data, uint32_t len);

/* Read up to len bytes from a file into buf starting at offset.
 * Returns bytes actually read. */
uint32_t RamFS_Read(RamFsNode *node, uint32_t offset,
                    uint8_t *buf, uint32_t len);

/* Delete a node (must be empty if directory). Returns 0 on success. */
int RamFS_Delete(RamFsVol *vol, const char *path);

/* Return the first child of a directory node (for iteration).
 * Use node->next_sibling to walk all siblings. */
RamFsNode *RamFS_FirstChild(RamFsNode *dir);

/* Allocate bytes from the shared pool (bump allocator). Returns NULL if full. */
uint8_t *RamFS_AllocPool(uint32_t bytes);

/* Get attributes of a node. Returns RAMFS_ATTR_* bit flags. */
uint8_t RamFS_GetAttrs(RamFsNode *node);

/* Set attributes of a node. Returns 0 on success, -1 if node is NULL. */
int RamFS_SetAttrs(RamFsNode *node, uint8_t attrs);

/* Get protection bits of a node. */
uint16_t RamFS_GetProtection(RamFsNode *node);

/* Set protection bits of a node. Returns 0 on success, -1 if node is NULL. */
int RamFS_SetProtection(RamFsNode *node, uint16_t prot);

/* Rename a volume. Returns 0 on success, -1 if not found. */
int RamFS_RenameVol(RamFsVol *vol, const char *new_name);

/* Rename a node (move within the same volume). Returns 0 on success. */
int RamFS_Rename(RamFsVol *vol, const char *old_path, const char *new_path);

/* Get volume capacity statistics.
 * total = total pool bytes, used = sum of all file data sizes. */
void RamFS_GetVolumeStats(RamFsVol *vol, uint32_t *total_bytes, uint32_t *used_bytes);

#endif
