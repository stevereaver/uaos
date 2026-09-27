/* ffs_handler.c — AmigaDOS packet handler for OFS/FFS block devices
 *
 * Wraps the FFS driver (ffs.c) in the Handler/DoPkt model, mirroring
 * fat_handler.c.  Read opens get read-only handles; FINDUPDATE and
 * FINDOUTPUT produce writable handles backed by FFS_WriteAt, and the
 * structural mutations (create/delete/rename/mkdir/metadata) route to
 * the driver's write path.
 */

#include "ffs_handler.h"
#include "dos/dospacket.h"
#include "dos/amiga_dos_types.h"
#include "dos/handle_table.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Open-file handles (key + byte offset; header block is re-read on demand)
 * ------------------------------------------------------------------------- */
#define FFS_MAX_FILES  16
#define FFS_MAX_LOCKS  16

static struct {
    uint32_t key;
    uint32_t pos;
    uint32_t size;
    int      writable;
    int      in_use;
} g_ffs_files[FFS_MAX_FILES];

static uint32_t ffs_alloc_file(uint32_t key, uint32_t size, int writable)
{
    for (int i = 0; i < FFS_MAX_FILES; i++) {
        if (!g_ffs_files[i].in_use) {
            g_ffs_files[i].in_use = 1;
            g_ffs_files[i].key = key;
            g_ffs_files[i].pos = 0;
            g_ffs_files[i].size = size;
            g_ffs_files[i].writable = writable;
            return (uint32_t)(i + 1);
        }
    }
    return 0;
}

static int ffs_get_file(uint32_t handle, uint32_t *key, uint32_t *pos,
                        uint32_t *size)
{
    if (handle == 0 || handle > FFS_MAX_FILES) return 0;
    if (!g_ffs_files[handle - 1].in_use) return 0;
    if (key)  *key  = g_ffs_files[handle - 1].key;
    if (pos)  *pos  = g_ffs_files[handle - 1].pos;
    if (size) *size = g_ffs_files[handle - 1].size;
    return 1;
}

static void ffs_free_file(uint32_t handle)
{
    if (handle == 0 || handle > FFS_MAX_FILES) return;
    g_ffs_files[handle - 1].in_use = 0;
}

/* -------------------------------------------------------------------------
 * Lock nodes — directory blocks plus enumeration cursor
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t   key;
    FfsDirIter iter;
    int        in_use;
} FfsLockNode;

static FfsLockNode g_ffs_locks[FFS_MAX_LOCKS];

static FfsLockNode *ffs_alloc_lock(uint32_t key)
{
    for (int i = 0; i < FFS_MAX_LOCKS; i++) {
        if (!g_ffs_locks[i].in_use) {
            g_ffs_locks[i].in_use = 1;
            g_ffs_locks[i].key = key;
            FFS_DirIterInit(&g_ffs_locks[i].iter);
            return &g_ffs_locks[i];
        }
    }
    return NULL;
}

static void ffs_free_lock_node(FfsLockNode *n)
{
    if (n) n->in_use = 0;
}

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/* Packet paths arrive as "vol:rel/path" — strip the volume prefix. */
static const char *ffs_rel_path(const char *path)
{
    if (!path) return "";
    const char *p = path;
    while (*p && *p != ':') p++;
    return (*p == ':') ? p + 1 : path;
}

static void ffs_fill_fib(FfsVolume *vol, const FfsEntry *e, FileInfoBlock *fib)
{
    memset(fib, 0, sizeof(*fib));
    int32_t st = e->sec_type;
    fib->fib_DirEntryType = st;
    fib->fib_EntryType    = st;
    int i = 0;
    while (i < 107 && e->name[i]) { fib->fib_FileName[i] = e->name[i]; i++; }
    fib->fib_FileName[i] = '\0';
    for (i = 0; i < 79 && e->comment[i]; i++)
        fib->fib_Comment[i] = e->comment[i];
    fib->fib_Comment[i] = '\0';
    fib->fib_Size       = (int32_t)e->byte_size;
    uint32_t payload    = vol->is_ofs ? FFS_OFS_PAYLOAD : vol->block_size;
    fib->fib_NumBlocks  = (int32_t)((e->byte_size + payload - 1) / payload);
    fib->fib_Protection = (int32_t)e->protect;
    fib->fib_Date.ds_Days   = (int32_t)e->days;
    fib->fib_Date.ds_Minute = (int32_t)e->mins;
    fib->fib_Date.ds_Tick   = (int32_t)e->ticks;
}

/* -------------------------------------------------------------------------
 * Packet processor
 * ------------------------------------------------------------------------- */
static void FfsHandler_ProcessPacket(Handler *h, DosPacket *pkt)
{
    FfsVolume *vol = (FfsVolume *)h->private;

    switch (pkt->dp_Type) {

    /* ===== File open / create ===== */
    case ACTION_FINDINPUT:
    case ACTION_FINDUPDATE:
    case ACTION_FINDOUTPUT: {
        const char *path = ffs_rel_path((const char *)(intptr_t)pkt->dp_Arg1);
        int want_write = (pkt->dp_Type != ACTION_FINDINPUT);
        uint32_t key;

        if (pkt->dp_Type == ACTION_FINDOUTPUT) {
            /* Create or truncate. */
            key = FFS_CreateFile(vol, path);
        } else {
            key = FFS_Resolve(vol, path);
            /* FINDUPDATE opens read/write — create when missing
             * (matches fat_handler/VFS_OPEN semantics). */
            if (!key && pkt->dp_Type == ACTION_FINDUPDATE)
                key = FFS_CreateFile(vol, path);
        }
        if (!key) {
            pkt->dp_Res1 = 0;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
            break;
        }
        FfsEntry e;
        if (FFS_ReadEntry(vol, key, &e) != 0) {
            pkt->dp_Res1 = 0;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
            break;
        }
        if (e.sec_type != FFS_ST_FILE && e.sec_type != FFS_ST_LINKFILE) {
            pkt->dp_Res1 = 0;
            pkt->dp_Res2 = ERROR_OBJECT_WRONG_TYPE;
            break;
        }
        uint32_t handle = ffs_alloc_file(key, e.byte_size, want_write);
        pkt->dp_Res1 = (int32_t)handle;
        if (handle == 0) pkt->dp_Res2 = ERROR_NO_FREE_STORE;
        break;
    }

    /* ===== Read ===== */
    case ACTION_READ: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        void *buf = (void *)(intptr_t)pkt->dp_Arg2;
        uint32_t len = (uint32_t)pkt->dp_Arg3;
        uint32_t key, pos, size;
        if (!ffs_get_file(handle, &key, &pos, &size)) {
            pkt->dp_Res1 = -1;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
            break;
        }
        FfsEntry e;
        if (FFS_ReadEntry(vol, key, &e) != 0) {
            pkt->dp_Res1 = -1;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
            break;
        }
        int32_t got = FFS_ReadAt(vol, &e, pos, buf, len);
        if (got < 0) {
            pkt->dp_Res1 = -1;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        } else {
            g_ffs_files[handle - 1].pos += (uint32_t)got;
            pkt->dp_Res1 = got;
        }
        break;
    }

    /* ===== Write ===== */
    case ACTION_WRITE: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        const void *buf = (const void *)(intptr_t)pkt->dp_Arg2;
        uint32_t len = (uint32_t)pkt->dp_Arg3;
        uint32_t key, pos, size;
        if (!ffs_get_file(handle, &key, &pos, &size) ||
            !g_ffs_files[handle - 1].writable) {
            pkt->dp_Res1 = -1;
            pkt->dp_Res2 = ERROR_WRITE_PROTECTED;
            break;
        }
        int32_t put = FFS_WriteAt(vol, key, pos, buf, len);
        if (put < 0) {
            pkt->dp_Res1 = -1;
            pkt->dp_Res2 = ERROR_DISK_FULL;
        } else {
            g_ffs_files[handle - 1].pos += (uint32_t)put;
            if (pos + (uint32_t)put > g_ffs_files[handle - 1].size)
                g_ffs_files[handle - 1].size = pos + (uint32_t)put;
            pkt->dp_Res1 = put;
            if (put < (int32_t)len) pkt->dp_Res2 = ERROR_DISK_FULL;
        }
        break;
    }

    /* ===== Close ===== */
    case ACTION_END: {
        ffs_free_file((uint32_t)pkt->dp_Arg1);
        pkt->dp_Res1 = 0;
        break;
    }

    /* ===== Seek ===== */
    case ACTION_SEEK: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        int32_t offset = pkt->dp_Arg2;
        int32_t mode = pkt->dp_Arg3;
        uint32_t key, pos, size;
        if (!ffs_get_file(handle, &key, &pos, &size)) {
            pkt->dp_Res1 = -1;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
            break;
        }
        int64_t np = pos;
        if (mode == OFFSET_CURRENT)         np = (int64_t)pos + offset;
        else if (mode == OFFSET_END)        np = (int64_t)size + offset;
        else                              np = offset; /* OFFSET_BEGINNING */
        if (np < 0) np = 0;
        if (np > size) np = size;
        g_ffs_files[handle - 1].pos = (uint32_t)np;
        pkt->dp_Res1 = (int32_t)pos;
        break;
    }

    /* ===== Lock / locate object ===== */
    case ACTION_LOCATE_OBJECT: {
        const char *path = ffs_rel_path((const char *)(intptr_t)pkt->dp_Arg1);
        int32_t access = pkt->dp_Arg2;
        uint32_t key = FFS_Resolve(vol, path);
        if (!key) {
            pkt->dp_Res1 = 0;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
            break;
        }
        FfsLockNode *node = ffs_alloc_lock(key);
        if (!node) {
            pkt->dp_Res1 = 0;
            pkt->dp_Res2 = ERROR_NO_FREE_STORE;
            break;
        }
        uint32_t handle = HandleTable_AllocLock(path, node, access);
        pkt->dp_Res1 = (int32_t)handle;
        if (handle == 0) {
            ffs_free_lock_node(node);
            pkt->dp_Res2 = ERROR_NO_FREE_STORE;
        }
        break;
    }

    /* ===== Free lock ===== */
    case ACTION_FREE_LOCK: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        HandleEntry *le = HandleTable_GetLockEntry(handle, NULL);
        if (le) ffs_free_lock_node((FfsLockNode *)le->u.lock.node);
        HandleTable_Free(handle);
        pkt->dp_Res1 = DOSTRUE;
        break;
    }

    /* ===== Examine object ===== */
    case ACTION_EXAMINE_OBJECT: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        FileInfoBlock *fib = (FileInfoBlock *)(intptr_t)pkt->dp_Arg2;
        HandleEntry *le = HandleTable_GetLockEntry(handle, NULL);
        FfsLockNode *node = le ? (FfsLockNode *)le->u.lock.node : NULL;
        uint32_t key = node ? node->key : 0;
        uint32_t fkey, fpos, fsize;
        if (!node && ffs_get_file(handle, &fkey, &fpos, &fsize)) key = fkey;
        FfsEntry e;
        if (key && FFS_ReadEntry(vol, key, &e) == 0) {
            ffs_fill_fib(vol, &e, fib);
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Examine next (directory listing) ===== */
    case ACTION_EXAMINE_NEXT:
    case ACTION_EXAMINE_ALL: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        FileInfoBlock *fib = (FileInfoBlock *)(intptr_t)pkt->dp_Arg2;
        HandleEntry *le = HandleTable_GetLockEntry(handle, NULL);
        FfsLockNode *node = le ? (FfsLockNode *)le->u.lock.node : NULL;
        if (!node) {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES;
            break;
        }
        FfsEntry e;
        if (FFS_DirIterNext(vol, node->key, &node->iter, &e)) {
            ffs_fill_fib(vol, &e, fib);
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES;
        }
        break;
    }

    /* ===== Examine file handle ===== */
    case ACTION_EXAMINE_FH: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        FileInfoBlock *fib = (FileInfoBlock *)(intptr_t)pkt->dp_Arg2;
        uint32_t key, pos, size;
        FfsEntry e;
        if (ffs_get_file(handle, &key, &pos, &size) &&
            FFS_ReadEntry(vol, key, &e) == 0) {
            ffs_fill_fib(vol, &e, fib);
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Disk info ===== */
    case ACTION_DISK_INFO:
    case ACTION_INFO: {
        InfoData *id = (InfoData *)(intptr_t)pkt->dp_Arg2;
        if (!id) {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
            break;
        }
        id->id_NumBlocks     = (int32_t)vol->num_blocks;
        id->id_NumBlocksUsed = (int32_t)vol->used_blocks;
        id->id_BytesPerBlock = (int32_t)vol->block_size;
        id->id_DiskState     = ID_VALIDATED;
        id->id_NumSoftErrors = 0;
        id->id_UnitNumber    = 0;
        id->id_DiskType      = (int32_t)FFS_DosType(vol);
        id->id_VolumeNode    = 0;
        id->id_InUse         = 1;
        pkt->dp_Res1 = DOSTRUE;
        break;
    }

    /* ===== Parent ===== */
    case ACTION_PARENT: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        HandleEntry *le = HandleTable_GetLockEntry(handle, NULL);
        FfsLockNode *node = le ? (FfsLockNode *)le->u.lock.node : NULL;
        pkt->dp_Res1 = 0;
        if (node) {
            FfsEntry e;
            if (FFS_ReadEntry(vol, node->key, &e) == 0 && e.parent_key) {
                FfsLockNode *par = ffs_alloc_lock(e.parent_key);
                if (par) {
                    uint32_t ph = HandleTable_AllocLock(le->path, par,
                                                        SHARED_LOCK);
                    if (ph) pkt->dp_Res1 = (int32_t)ph;
                    else ffs_free_lock_node(par);
                }
            }
        }
        break;
    }

    /* ===== DupLock ===== */
    case ACTION_COPY_DIR: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        int32_t access = 0;
        HandleEntry *le = HandleTable_GetLockEntry(handle, &access);
        FfsLockNode *node = le ? (FfsLockNode *)le->u.lock.node : NULL;
        pkt->dp_Res1 = 0;
        if (node) {
            FfsLockNode *dup = ffs_alloc_lock(node->key);
            if (dup) {
                uint32_t ph = HandleTable_AllocLock(le->path, dup, access);
                if (ph) pkt->dp_Res1 = (int32_t)ph;
                else {
                    ffs_free_lock_node(dup);
                    pkt->dp_Res2 = ERROR_NO_FREE_STORE;
                }
            } else {
                pkt->dp_Res2 = ERROR_NO_FREE_STORE;
            }
        }
        break;
    }

    /* ===== Same lock ===== */
    case ACTION_SAME_LOCK: {
        uint32_t h1 = (uint32_t)pkt->dp_Arg1;
        uint32_t h2 = (uint32_t)pkt->dp_Arg2;
        HandleEntry *le1 = HandleTable_GetLockEntry(h1, NULL);
        HandleEntry *le2 = HandleTable_GetLockEntry(h2, NULL);
        FfsLockNode *n1 = le1 ? (FfsLockNode *)le1->u.lock.node : NULL;
        FfsLockNode *n2 = le2 ? (FfsLockNode *)le2->u.lock.node : NULL;
        pkt->dp_Res1 = (n1 && n2 && n1->key == n2->key) ? DOSTRUE : DOSFALSE;
        break;
    }

    /* ===== Is filesystem ===== */
    case ACTION_IS_FILESYSTEM: {
        pkt->dp_Res1 = DOSTRUE;
        break;
    }

    /* ===== Delete ===== */
    case ACTION_DELETE_OBJECT: {
        const char *path = ffs_rel_path((const char *)(intptr_t)pkt->dp_Arg1);
        int r = FFS_Delete(vol, path);
        if (r == 0) {
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = (r == -3) ? ERROR_OBJECT_WRONG_TYPE
                                     : ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Create directory ===== */
    case ACTION_CREATE_DIR: {
        const char *path = ffs_rel_path((const char *)(intptr_t)pkt->dp_Arg1);
        uint32_t key = FFS_CreateDir(vol, path);
        if (!key) {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
            break;
        }
        FfsLockNode *node = ffs_alloc_lock(key);
        if (!node) {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_NO_FREE_STORE;
            break;
        }
        uint32_t handle = HandleTable_AllocLock(path, node, SHARED_LOCK);
        pkt->dp_Res1 = (int32_t)handle;
        if (handle == 0) {
            ffs_free_lock_node(node);
            pkt->dp_Res2 = ERROR_NO_FREE_STORE;
        }
        break;
    }

    /* ===== Rename ===== */
    case ACTION_RENAME_OBJECT: {
        const char *oldp = ffs_rel_path((const char *)(intptr_t)pkt->dp_Arg1);
        const char *newp = ffs_rel_path((const char *)(intptr_t)pkt->dp_Arg2);
        int r = FFS_Rename(vol, oldp, newp);
        if (r == 0) {
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = (r == -2) ? ERROR_OBJECT_EXISTS
                                     : ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Set protection ===== */
    case ACTION_SET_PROTECT: {
        const char *path = ffs_rel_path((const char *)(intptr_t)pkt->dp_Arg1);
        uint32_t prot = (uint32_t)pkt->dp_Arg2;
        uint32_t key = FFS_Resolve(vol, path);
        if (key && FFS_SetProtect(vol, key, prot) == 0) {
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Set comment ===== */
    case ACTION_SET_COMMENT: {
        const char *path = ffs_rel_path((const char *)(intptr_t)pkt->dp_Arg1);
        const char *comment = (const char *)(intptr_t)pkt->dp_Arg2;
        uint32_t key = FFS_Resolve(vol, path);
        if (key && FFS_SetComment(vol, key, comment) == 0) {
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Set datestamp ===== */
    case ACTION_SET_DATE: {
        const char *path = ffs_rel_path((const char *)(intptr_t)pkt->dp_Arg1);
        const int32_t *ds = (const int32_t *)(intptr_t)pkt->dp_Arg2;
        uint32_t key = FFS_Resolve(vol, path);
        uint32_t d = ds ? (uint32_t)ds[0] : 0;
        uint32_t m = ds ? (uint32_t)ds[1] : 0;
        uint32_t t = ds ? (uint32_t)ds[2] : 0;
        if (key && FFS_SetDate(vol, key, d, m, t) == 0) {
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Set file size ===== */
    case ACTION_SET_FILE_SIZE: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        uint32_t nsize = (uint32_t)pkt->dp_Arg2;
        uint32_t key, pos, size;
        if (ffs_get_file(handle, &key, &pos, &size) &&
            g_ffs_files[handle - 1].writable &&
            FFS_SetFileSize(vol, key, nsize) == 0) {
            g_ffs_files[handle - 1].size = nsize;
            if (g_ffs_files[handle - 1].pos > nsize)
                g_ffs_files[handle - 1].pos = nsize;
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Flush — I/O is synchronous, nothing cached ===== */
    case ACTION_FLUSH:
        pkt->dp_Res1 = DOSTRUE;
        break;

    case ACTION_PARENT_FH:
    case ACTION_INHIBIT:
    default:
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
        break;
    }
}

int FfsHandler_Is(const Handler *handler)
{
    return handler && handler->ProcessPacket == FfsHandler_ProcessPacket;
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */
Handler *FfsHandler_Create(const char *name, FfsVolume *vol)
{
    if (!vol) return NULL;
    return Handler_Create(name, vol, FfsHandler_ProcessPacket);
}
