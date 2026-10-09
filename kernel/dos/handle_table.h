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
#define HTYPE_FREE    0
#define HTYPE_FILE    1
#define HTYPE_LOCK    2
#define HTYPE_MEMFILE 3   /* read-only file image living in guest RAM
                           * (overlay/SFX executables keep their own
                           * executable open for Seek/Read — see
                           * hunk_load in emulation/uaos_m68k_glue.c) */

/* Per-handle entry */
typedef struct {
    uint8_t  type;          /* HTYPE_* */
    char     path[128];   /* path string (diagnostic / re-open) */
    void    *owner;         /* UaosTask that allocated (leak detection) */
    union {
        struct {
            VfsFile   fh;     /* copy of open file handle */
            int       flags;  /* VFS_READ / VFS_WRITE etc. */
        } file;
        struct {
            void      *node;       /* locked node (RamFsNode*, Fat32File*, etc.) */
            int32_t    access;     /* SHARED_LOCK / EXCLUSIVE_LOCK */
            void      *iter_next;  /* next child for ExamineNext (handler-specific) */
        } lock;
        struct {
            uint32_t   ram_addr;     /* guest RAM offset of the file image */
            uint32_t   size;         /* image size in bytes                */
            uint32_t   pos;          /* current read position              */
        } memfile;
    } u;
} HandleEntry;

/* Initialise table. Call once at boot. */
void HandleTable_Init(void);

/* Allocate a file handle.  Returns 0 on failure, non-zero handle on success.
 * The VfsFile is copied into internal storage. */
uint32_t HandleTable_AllocFile(const char *path, const VfsFile *fh, int flags);

/* Allocate a lock handle.  Returns 0 on failure. */
uint32_t HandleTable_AllocLock(const char *path, void *node, int32_t access);

/* Allocate a memory-backed file handle over a guest-RAM image.
 * Read/Seek are serviced from g_ram[ram_addr..ram_addr+size); there is no
 * VFS object behind it.  Returns 0 on failure. */
uint32_t HandleTable_AllocMemFile(const char *path, uint32_t ram_addr,
                                  uint32_t size);

/* Free a handle (any type). */
void HandleTable_Free(uint32_t handle);

/* Look up entry by handle.  Returns NULL if invalid or freed. */
HandleEntry *HandleTable_Get(uint32_t handle);

/* Convenience: get file handle VfsFile*.  Returns NULL if not a file handle. */
VfsFile *HandleTable_GetFile(uint32_t handle);

/* Free every handle owned by `owner` (task teardown).  Returns the count. */
uint32_t HandleTable_FreeByOwner(void *owner);

/* Convenience: get lock entry.  Returns NULL if not a lock.
 * If access_out is non-NULL, writes the lock's access mode. */
HandleEntry *HandleTable_GetLockEntry(uint32_t handle, int32_t *access_out);

/* Convenience: advance lock iterator and return next child.
 * Returns NULL when there are no more entries.
 * The caller must cast iter_next to the appropriate type. */
/* C:handles — dump the table (read-only). */
void HandleTable_DiagDump(void *ctx, void (*emit)(void *, const char *));

void *HandleTable_LockIterate(uint32_t handle);

/* Reset lock iterator to first child (handler-specific). */
void HandleTable_LockResetIter(uint32_t handle, void *first_child);

#endif
