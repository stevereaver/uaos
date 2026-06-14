/* handle_table.h — Global open-file and lock handle table
 *
 * Both native VFS clients and M68k emulation glue allocate handles here.
 * A handle is an opaque integer ID (suitable for use as a BPTR in guest
 * code after shifting).  The underlying storage lives in this module.
 */

#ifndef UAOS_HANDLE_TABLE_H
#define UAOS_HANDLE_TABLE_H

#include <stdint.h>
#include "dos/vfs.h"
#include "dos/ramfs.h"

/* Handle type tags */
#define HTYPE_FREE   0
#define HTYPE_FILE   1
#define HTYPE_LOCK   2

/* Per-handle entry */
typedef struct {
    uint8_t  type;          /* HTYPE_* */
    char     path[128];   /* path string (diagnostic / re-open) */
    union {
        struct {
            VfsFile   fh;     /* copy of open file handle */
            int       flags;  /* VFS_READ / VFS_WRITE etc. */
        } file;
        struct {
            RamFsNode *node;       /* locked directory node */
            int32_t    access;     /* SHARED_LOCK / EXCLUSIVE_LOCK */
            RamFsNode *iter_next;  /* next child for ExamineNext */
        } lock;
    } u;
} HandleEntry;

/* Initialise table. Call once at boot. */
void HandleTable_Init(void);

/* Allocate a file handle.  Returns 0 on failure, non-zero handle on success.
 * The VfsFile is copied into internal storage. */
uint32_t HandleTable_AllocFile(const char *path, const VfsFile *fh, int flags);

/* Allocate a lock handle.  Returns 0 on failure. */
uint32_t HandleTable_AllocLock(const char *path, RamFsNode *node, int32_t access);

/* Free a handle (any type). */
void HandleTable_Free(uint32_t handle);

/* Look up entry by handle.  Returns NULL if invalid or freed. */
HandleEntry *HandleTable_Get(uint32_t handle);

/* Convenience: get file handle VfsFile*.  Returns NULL if not a file handle. */
VfsFile *HandleTable_GetFile(uint32_t handle);

/* Convenience: get lock node.  Returns NULL if not a lock.
 * If access_out is non-NULL, writes the lock's access mode. */
RamFsNode *HandleTable_GetLock(uint32_t handle, int32_t *access_out);

/* Convenience: advance lock iterator and return next child.
 * Returns NULL when there are no more entries. */
RamFsNode *HandleTable_LockIterate(uint32_t handle);

/* Reset lock iterator to first child. */
void HandleTable_LockResetIter(uint32_t handle);

#endif
