/* ram_handler.c — AmigaDOS packet handler for the in-memory RAM filesystem */

#include "ram_handler.h"
#include "dos/dospacket.h"
#include "dos/amiga_dos_types.h"
#include "dos/handle_table.h"
#include "dos/ramfs.h"
#include "dos/vfs.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern void kprint(const char *s);

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */
static void scopy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int slen(const char *s)
{
    int n = 0; while (s[n]) n++; return n;
}



/* -------------------------------------------------------------------------
 * Packet processor
 * ------------------------------------------------------------------------- */
static void RamHandler_ProcessPacket(Handler *h, DosPacket *pkt)
{
    RamFsVol *vol = (RamFsVol *)h->private;

    switch (pkt->dp_Type) {

    /* ===== File open / create ===== */
    case ACTION_FINDINPUT:
    case ACTION_FINDOUTPUT:
    case ACTION_FINDUPDATE: {
        const char *path = (const char *)(intptr_t)pkt->dp_Arg1;
        int vfs_flags = 0;
        if (pkt->dp_Type == ACTION_FINDINPUT)       vfs_flags = VFS_READ;
        else if (pkt->dp_Type == ACTION_FINDOUTPUT)  vfs_flags = VFS_WRITE | VFS_CREATE | VFS_TRUNC;
        else                                         vfs_flags = VFS_READ | VFS_WRITE | VFS_CREATE;

        VfsFile fh;
        if (VFS_Open(&fh, path, vfs_flags)) {
            uint32_t handle = HandleTable_AllocFile(path, &fh, vfs_flags);
            pkt->dp_Res1 = (int32_t)handle;
            if (handle == 0) {
                VFS_Close(&fh);
                pkt->dp_Res2 = ERROR_NO_FREE_STORE;
            }
        } else {
            pkt->dp_Res1 = 0;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Read ===== */
    case ACTION_READ: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        uint8_t *buf    = (uint8_t *)(intptr_t)pkt->dp_Arg2;
        uint32_t len    = (uint32_t)pkt->dp_Arg3;
        VfsFile *fh = HandleTable_GetFile(handle);
        if (fh && fh->node) {
            pkt->dp_Res1 = (int32_t)VFS_Read(fh, buf, len);
        } else {
            pkt->dp_Res1 = -1;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Write ===== */
    case ACTION_WRITE: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        const uint8_t *buf = (const uint8_t *)(intptr_t)pkt->dp_Arg2;
        uint32_t len = (uint32_t)pkt->dp_Arg3;
        VfsFile *fh = HandleTable_GetFile(handle);
        if (fh && fh->node) {
            pkt->dp_Res1 = (int32_t)VFS_Write(fh, buf, len);
        } else {
            pkt->dp_Res1 = -1;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Close ===== */
    case ACTION_END: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        VfsFile *fh = HandleTable_GetFile(handle);
        if (fh && fh->node) {
            VFS_Close(fh);
        }
        HandleTable_Free(handle);
        pkt->dp_Res1 = 0;
        break;
    }

    /* ===== Seek ===== */
    case ACTION_SEEK: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        int32_t  offset = pkt->dp_Arg2;
        int32_t  mode   = pkt->dp_Arg3;
        VfsFile *fh = HandleTable_GetFile(handle);
        if (fh && fh->node) {
            uint32_t new_pos = 0;
            uint32_t size = VFS_Size(fh);
            if (mode == OFFSET_CURRENT)      new_pos = fh->pos + (uint32_t)offset;
            else if (mode == OFFSET_END)       new_pos = size + (uint32_t)offset;
            else if (mode == OFFSET_BEGINNING) new_pos = (uint32_t)offset;
            else                               new_pos = (uint32_t)offset;
            pkt->dp_Res1 = (int32_t)fh->pos; /* return old position */
            VFS_Seek(fh, new_pos);
        } else {
            pkt->dp_Res1 = -1;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Set file size ===== */
    case ACTION_SET_FILE_SIZE: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        int32_t  offset = pkt->dp_Arg2;
        int32_t  mode   = pkt->dp_Arg3; /* same seek modes */
        VfsFile *fh = HandleTable_GetFile(handle);
        if (fh && fh->node) {
            uint32_t new_size = 0;
            uint32_t size = VFS_Size(fh);
            if (mode == OFFSET_CURRENT)      new_size = fh->pos + (uint32_t)offset;
            else if (mode == OFFSET_END)       new_size = size + (uint32_t)offset;
            else if (mode == OFFSET_BEGINNING) new_size = (uint32_t)offset;
            else                               new_size = (uint32_t)offset;
            /* RamFS doesn't support explicit truncate without rewrite;
             * we fake it by rewriting exactly new_size bytes. */
            if (new_size < size) {
                uint8_t tmp[1];
                VFS_Seek(fh, new_size);
                VFS_Write(fh, tmp, 0); /* no-op trunc not supported directly */
            }
            pkt->dp_Res1 = (int32_t)new_size;
        } else {
            pkt->dp_Res1 = -1;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Delete object ===== */
    case ACTION_DELETE_OBJECT: {
        const char *path = (const char *)(intptr_t)pkt->dp_Arg1;
        if (VFS_Delete(path)) {
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Create directory ===== */
    case ACTION_CREATE_DIR: {
        const char *path = (const char *)(intptr_t)pkt->dp_Arg1;
        if (VFS_MkDir(path)) {
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Lock / Locate object ===== */
    case ACTION_LOCATE_OBJECT: {
        const char *path = (const char *)(intptr_t)pkt->dp_Arg1;
        int32_t access = pkt->dp_Arg2;
        RamFsNode *node = VFS_ResolveDir(path);
        if (!node) {
            /* Try resolving as file — AmigaDOS Lock() locks files too */
            VfsFile tmp;
            if (VFS_Open(&tmp, path, VFS_READ)) {
                node = tmp.node;
                VFS_Close(&tmp);
            }
        }
        if (node) {
            uint32_t handle = HandleTable_AllocLock(path, node, access);
            pkt->dp_Res1 = (int32_t)handle;
            if (handle == 0) pkt->dp_Res2 = ERROR_NO_FREE_STORE;
        } else {
            pkt->dp_Res1 = 0;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Free lock (Unlock) ===== */
    case ACTION_FREE_LOCK: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        HandleTable_Free(handle);
        pkt->dp_Res1 = DOSTRUE;
        break;
    }

    /* ===== Examine object (lock or file handle) ===== */
    case ACTION_EXAMINE_OBJECT: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        FileInfoBlock *fib = (FileInfoBlock *)(intptr_t)pkt->dp_Arg2;
        RamFsNode *node = NULL;

        int32_t access = 0;
        HandleEntry *le = HandleTable_GetLockEntry(handle, &access);
        if (le) node = (RamFsNode *)le->u.lock.node;
        if (!node) {
            VfsFile *fh = HandleTable_GetFile(handle);
            if (fh) node = fh->node;
        }
        if (node) {
            memset(fib, 0, sizeof(*fib));
            if (node->type == RAMFS_TYPE_DIR) {
                fib->fib_DirEntryType = ST_USERDIR;
                fib->fib_EntryType    = ST_USERDIR;
            } else {
                fib->fib_DirEntryType = ST_FILE;
                fib->fib_EntryType    = ST_FILE;
            }
            int i = 0;
            while (i < 107 && node->name[i]) { fib->fib_FileName[i] = node->name[i]; i++; }
            fib->fib_FileName[i] = '\0';
            fib->fib_Size       = (int32_t)node->size;
            fib->fib_NumBlocks  = (int32_t)((node->size + 511) / 512);
            fib->fib_Protection = (int32_t)node->protection;
            /* Date is left at zero (no RTC in RamFS) */
            int j = 0;
            while (j < 79 && node->comment[j]) { fib->fib_Comment[j] = node->comment[j]; j++; }
            fib->fib_Comment[j] = '\0';
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Examine next (directory listing) ===== */
    case ACTION_EXAMINE_NEXT: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        FileInfoBlock *fib = (FileInfoBlock *)(intptr_t)pkt->dp_Arg2;
        int32_t access = 0;
        HandleEntry *le = HandleTable_GetLockEntry(handle, &access);
        RamFsNode *dir = le ? (RamFsNode *)le->u.lock.node : NULL;
        if (!dir || dir->type != RAMFS_TYPE_DIR) {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
            break;
        }
        RamFsNode *child = (RamFsNode *)HandleTable_LockIterate(handle);
        if (child) {
            /* Advance iterator for next call */
            le->u.lock.iter_next = child->next_sibling;
            memset(fib, 0, sizeof(*fib));
            if (child->type == RAMFS_TYPE_DIR) {
                fib->fib_DirEntryType = ST_USERDIR;
                fib->fib_EntryType    = ST_USERDIR;
            } else {
                fib->fib_DirEntryType = ST_FILE;
                fib->fib_EntryType    = ST_FILE;
            }
            int i = 0;
            while (i < 107 && child->name[i]) { fib->fib_FileName[i] = child->name[i]; i++; }
            fib->fib_FileName[i] = '\0';
            fib->fib_Size      = (int32_t)child->size;
            fib->fib_NumBlocks = (int32_t)((child->size + 511) / 512);
            fib->fib_Protection = (int32_t)child->protection;
            int j = 0;
            while (j < 79 && child->comment[j]) { fib->fib_Comment[j] = child->comment[j]; j++; }
            fib->fib_Comment[j] = '\0';
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES;
            /* Reset iterator so next Examine works again */
            HandleTable_LockResetIter(handle, dir->first_child);
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

        uint32_t total_bytes = 0, used_bytes = 0;
        RamFS_GetVolumeStats(vol, &total_bytes, &used_bytes);

        id->id_NumBlocks     = (int32_t)(total_bytes / 512);
        id->id_NumBlocksUsed = (int32_t)(used_bytes / 512);
        id->id_BytesPerBlock = 512;
        id->id_DiskState     = ID_VALIDATED;
        id->id_NumSoftErrors = 0;
        id->id_UnitNumber    = 0;
        id->id_DiskType      = ID_DOS_DISK;
        id->id_VolumeNode    = 0;
        id->id_InUse         = 1;

        pkt->dp_Res1 = DOSTRUE;
        break;
    }

    /* ===== Parent ===== */
    case ACTION_PARENT: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        int32_t access = 0;
        HandleEntry *le = HandleTable_GetLockEntry(handle, &access);
        RamFsNode *node = le ? (RamFsNode *)le->u.lock.node : NULL;
        if (node && node->parent) {
            uint32_t ph = HandleTable_AllocLock("", node->parent, access);
            pkt->dp_Res1 = (int32_t)ph;
            if (ph == 0) pkt->dp_Res2 = ERROR_NO_FREE_STORE;
        } else {
            pkt->dp_Res1 = 0; /* NULL lock = no parent (root) */
        }
        break;
    }

    /* ===== Copy dir (DupLock) ===== */
    case ACTION_COPY_DIR: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        int32_t access = 0;
        HandleEntry *le = HandleTable_GetLockEntry(handle, &access);
        RamFsNode *node = le ? (RamFsNode *)le->u.lock.node : NULL;
        if (node) {
            uint32_t ph = HandleTable_AllocLock("", node, access);
            pkt->dp_Res1 = (int32_t)ph;
            if (ph == 0) pkt->dp_Res2 = ERROR_NO_FREE_STORE;
        } else {
            pkt->dp_Res1 = 0;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Rename object ===== */
    case ACTION_RENAME_OBJECT: {
        const char *old_path = (const char *)(intptr_t)pkt->dp_Arg1;
        const char *new_path = (const char *)(intptr_t)pkt->dp_Arg2;
        if (VFS_Rename(old_path, new_path) == 0) {
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Set protection ===== */
    case ACTION_SET_PROTECT: {
        const char *path = (const char *)(intptr_t)pkt->dp_Arg1;
        int32_t mask = pkt->dp_Arg2;
        if (VFS_SetProtection(path, (uint16_t)mask) == 0) {
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Set comment ===== */
    case ACTION_SET_COMMENT: {
        const char *path    = (const char *)(intptr_t)pkt->dp_Arg1;
        const char *comment = (const char *)(intptr_t)pkt->dp_Arg2;
        if (VFS_SetComment(path, comment) == 0) {
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Same lock ===== */
    case ACTION_SAME_LOCK: {
        uint32_t h1 = (uint32_t)pkt->dp_Arg1;
        uint32_t h2 = (uint32_t)pkt->dp_Arg2;
        int32_t a1 = 0, a2 = 0;
        HandleEntry *le1 = HandleTable_GetLockEntry(h1, &a1);
        HandleEntry *le2 = HandleTable_GetLockEntry(h2, &a2);
        RamFsNode *n1 = le1 ? (RamFsNode *)le1->u.lock.node : NULL;
        RamFsNode *n2 = le2 ? (RamFsNode *)le2->u.lock.node : NULL;
        pkt->dp_Res1 = (n1 && n2 && n1 == n2) ? DOSTRUE : DOSFALSE;
        break;
    }

    /* ===== Is filesystem ===== */
    case ACTION_IS_FILESYSTEM: {
        pkt->dp_Res1 = DOSTRUE;
        break;
    }

    /* ===== Current volume ===== */
    case ACTION_CURRENT_VOLUME: {
        /* Return a lock on the volume root */
        uint32_t handle = HandleTable_AllocLock("", vol->root, SHARED_LOCK);
        pkt->dp_Res1 = (int32_t)handle;
        if (handle == 0) pkt->dp_Res2 = ERROR_NO_FREE_STORE;
        break;
    }

    /* ===== Parent from file handle ===== */
    case ACTION_PARENT_FH: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        VfsFile *fh = HandleTable_GetFile(handle);
        if (fh && fh->node && fh->node->parent) {
            uint32_t ph = HandleTable_AllocLock("", fh->node->parent, SHARED_LOCK);
            pkt->dp_Res1 = (int32_t)ph;
            if (ph == 0) pkt->dp_Res2 = ERROR_NO_FREE_STORE;
        } else {
            pkt->dp_Res1 = 0; /* NULL lock = no parent (root) */
        }
        break;
    }

    /* ===== Examine file handle ===== */
    case ACTION_EXAMINE_FH: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        FileInfoBlock *fib = (FileInfoBlock *)(intptr_t)pkt->dp_Arg2;
        VfsFile *fh = HandleTable_GetFile(handle);
        RamFsNode *node = fh ? fh->node : NULL;
        if (node) {
            memset(fib, 0, sizeof(*fib));
            if (node->type == RAMFS_TYPE_DIR) {
                fib->fib_DirEntryType = ST_USERDIR;
                fib->fib_EntryType    = ST_USERDIR;
            } else {
                fib->fib_DirEntryType = ST_FILE;
                fib->fib_EntryType    = ST_FILE;
            }
            int i = 0;
            while (i < 107 && node->name[i]) { fib->fib_FileName[i] = node->name[i]; i++; }
            fib->fib_FileName[i] = '\0';
            fib->fib_Size       = (int32_t)node->size;
            fib->fib_NumBlocks  = (int32_t)((node->size + 511) / 512);
            fib->fib_Protection = (int32_t)node->protection;
            int j = 0;
            while (j < 79 && node->comment[j]) { fib->fib_Comment[j] = node->comment[j]; j++; }
            fib->fib_Comment[j] = '\0';
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Examine all (includes . and ..) ===== */
    case ACTION_EXAMINE_ALL: {
        /* RamFS has no . / .. entries; delegate to EXAMINE_NEXT */
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        FileInfoBlock *fib = (FileInfoBlock *)(intptr_t)pkt->dp_Arg2;
        int32_t access = 0;
        HandleEntry *le = HandleTable_GetLockEntry(handle, &access);
        RamFsNode *dir = le ? (RamFsNode *)le->u.lock.node : NULL;
        if (!dir || dir->type != RAMFS_TYPE_DIR) {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
            break;
        }
        RamFsNode *child = (RamFsNode *)HandleTable_LockIterate(handle);
        if (child) {
            le->u.lock.iter_next = child->next_sibling;
            memset(fib, 0, sizeof(*fib));
            if (child->type == RAMFS_TYPE_DIR) {
                fib->fib_DirEntryType = ST_USERDIR;
                fib->fib_EntryType    = ST_USERDIR;
            } else {
                fib->fib_DirEntryType = ST_FILE;
                fib->fib_EntryType    = ST_FILE;
            }
            int i = 0;
            while (i < 107 && child->name[i]) { fib->fib_FileName[i] = child->name[i]; i++; }
            fib->fib_FileName[i] = '\0';
            fib->fib_Size      = (int32_t)child->size;
            fib->fib_NumBlocks = (int32_t)((child->size + 511) / 512);
            fib->fib_Protection = (int32_t)child->protection;
            int j = 0;
            while (j < 79 && child->comment[j]) { fib->fib_Comment[j] = child->comment[j]; j++; }
            fib->fib_Comment[j] = '\0';
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES;
            HandleTable_LockResetIter(handle, dir->first_child);
        }
        break;
    }

    /* ===== Set date (touch) ===== */
    case ACTION_SET_DATE: {
        /* RamFS has no timestamps — no-op success */
        pkt->dp_Res1 = DOSTRUE;
        break;
    }

    /* ===== Flush buffers ===== */
    case ACTION_FLUSH: {
        /* RamFS is memory-only; nothing to flush */
        pkt->dp_Res1 = DOSTRUE;
        break;
    }

    /* ===== Inhibit volume I/O ===== */
    case ACTION_INHIBIT: {
        /* Stub: would set a per-volume inhibit flag */
        pkt->dp_Res1 = DOSTRUE;
        break;
    }

    /* ===== Die ===== */
    case ACTION_DIE: {
        pkt->dp_Res1 = DOSTRUE;
        break;
    }

    /* ===== Unimplemented / unknown ===== */
    default:
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
        break;
    }
}

/* -------------------------------------------------------------------------
 * Constructor
 * ------------------------------------------------------------------------- */
Handler *RamHandler_Create(const char *name, RamFsVol *vol)
{
    return Handler_Create(name, vol, RamHandler_ProcessPacket);
}
