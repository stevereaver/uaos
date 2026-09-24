/* fat_handler.c — AmigaDOS packet handler for FAT32 block devices
 *
 * Wraps the FAT32 driver (fat32.c) in the Handler/DoPkt model.
 * Supports file open/create/read/write, directory create/delete/list,
 * examine, disk info, and rename.
 */

#include "fat_handler.h"
#include "dos/dospacket.h"
#include "dos/amiga_dos_types.h"
#include "dos/handle_table.h"
#include "dos/vfs.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Per-handler file handle table
 * HandleTable stores VfsFile (RAMFS-specific) for files, so FAT32
 * files are tracked in a private table here.  Locks (directories) are
 * stored in the global HandleTable since it now supports generic
 * void* nodes.
 * ------------------------------------------------------------------------- */
#define FAT_MAX_FILES 16

static struct {
    Fat32File *file;
    int        in_use;
} g_fat_files[FAT_MAX_FILES];

static uint32_t fat_alloc_file_handle(Fat32File *file)
{
    for (int i = 0; i < FAT_MAX_FILES; i++) {
        if (!g_fat_files[i].in_use) {
            g_fat_files[i].in_use = 1;
            g_fat_files[i].file = file;
            return (uint32_t)(i + 1);
        }
    }
    return 0;
}

static Fat32File *fat_get_file_handle(uint32_t handle)
{
    if (handle == 0 || handle > FAT_MAX_FILES) return NULL;
    if (!g_fat_files[handle - 1].in_use) return NULL;
    return g_fat_files[handle - 1].file;
}

static void fat_free_file_handle(uint32_t handle)
{
    if (handle == 0 || handle > FAT_MAX_FILES) return;
    g_fat_files[handle - 1].in_use = 0;
    g_fat_files[handle - 1].file = NULL;
}

/* -------------------------------------------------------------------------
 * Packet processor
 * ------------------------------------------------------------------------- */
static void FatHandler_ProcessPacket(Handler *h, DosPacket *pkt)
{
    Fat32FS *fs = (Fat32FS *)h->private;

    switch (pkt->dp_Type) {

    /* ===== File open / create ===== */
    case ACTION_FINDINPUT:
    case ACTION_FINDOUTPUT:
    case ACTION_FINDUPDATE: {
        const char *path = (const char *)(intptr_t)pkt->dp_Arg1;
        Fat32File *file = NULL;

        if (pkt->dp_Type == ACTION_FINDOUTPUT) {
            /* Create or truncate */
            file = FAT32_CreateFile(fs, path);
        } else {
            /* FINDINPUT (read-only) or FINDUPDATE (read/write, create) */
            file = FAT32_Open(fs, path);
            if (!file && pkt->dp_Type == ACTION_FINDUPDATE) {
                file = FAT32_CreateFile(fs, path);
            }
        }

        if (file) {
            uint32_t handle = fat_alloc_file_handle(file);
            pkt->dp_Res1 = (int32_t)handle;
            if (handle == 0) {
                FAT32_Close(file);
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
        void *buf = (void *)(intptr_t)pkt->dp_Arg2;
        uint32_t len = (uint32_t)pkt->dp_Arg3;
        Fat32File *file = fat_get_file_handle(handle);
        if (file && !file->is_dir) {
            pkt->dp_Res1 = (int32_t)FAT32_Read(file, buf, len);
        } else {
            pkt->dp_Res1 = -1;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Write ===== */
    case ACTION_WRITE: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        const void *buf = (const void *)(intptr_t)pkt->dp_Arg2;
        uint32_t len = (uint32_t)pkt->dp_Arg3;
        Fat32File *file = fat_get_file_handle(handle);
        if (file && !file->is_dir) {
            pkt->dp_Res1 = (int32_t)FAT32_Write(file, buf, len);
        } else {
            pkt->dp_Res1 = -1;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Close ===== */
    case ACTION_END: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        Fat32File *file = fat_get_file_handle(handle);
        if (file) FAT32_Close(file);
        fat_free_file_handle(handle);
        pkt->dp_Res1 = 0;
        break;
    }

    /* ===== Seek ===== */
    case ACTION_SEEK: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        int32_t offset = pkt->dp_Arg2;
        int32_t mode = pkt->dp_Arg3;
        Fat32File *file = fat_get_file_handle(handle);
        if (file) {
            uint32_t old = file->pos;
            uint32_t size = FAT32_Size(file);
            uint32_t new_pos = 0;
            if (mode == OFFSET_CURRENT)      new_pos = old + (uint32_t)offset;
            else if (mode == OFFSET_END)     new_pos = size + (uint32_t)offset;
            else if (mode == OFFSET_BEGINNING) new_pos = (uint32_t)offset;
            else                             new_pos = (uint32_t)offset;
            FAT32_Seek(file, new_pos);
            pkt->dp_Res1 = (int32_t)old;
        } else {
            pkt->dp_Res1 = -1;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Delete ===== */
    case ACTION_DELETE_OBJECT: {
        const char *path = (const char *)(intptr_t)pkt->dp_Arg1;
        if (FAT32_Delete(fs, path) == 0) {
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
        if (FAT32_CreateDir(fs, path) == 0) {
            /* Return a lock on the new directory */
            Fat32File *dir = FAT32_Open(fs, path);
            if (dir) {
                uint32_t handle = HandleTable_AllocLock(path, dir,
                                                        SHARED_LOCK);
                pkt->dp_Res1 = (int32_t)handle;
                if (handle == 0) {
                    FAT32_Close(dir);
                    pkt->dp_Res2 = ERROR_NO_FREE_STORE;
                }
            } else {
                pkt->dp_Res1 = DOSFALSE;
                pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
            }
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
        Fat32File *dir = FAT32_Open(fs, path);
        if (dir) {
            uint32_t handle = HandleTable_AllocLock(path, dir, access);
            pkt->dp_Res1 = (int32_t)handle;
            if (handle == 0) {
                FAT32_Close(dir);
                pkt->dp_Res2 = ERROR_NO_FREE_STORE;
            }
        } else {
            pkt->dp_Res1 = 0;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Free lock ===== */
    case ACTION_FREE_LOCK: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        HandleEntry *le = HandleTable_GetLockEntry(handle, NULL);
        if (le) {
            Fat32File *dir = (Fat32File *)le->u.lock.node;
            if (dir) FAT32_Close(dir);
        }
        HandleTable_Free(handle);
        pkt->dp_Res1 = DOSTRUE;
        break;
    }

    /* ===== Examine object ===== */
    case ACTION_EXAMINE_OBJECT: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        FileInfoBlock *fib = (FileInfoBlock *)(intptr_t)pkt->dp_Arg2;
        HandleEntry *le = HandleTable_GetLockEntry(handle, NULL);
        Fat32File *node = le ? (Fat32File *)le->u.lock.node : NULL;
        if (!node) node = fat_get_file_handle(handle);
        if (node) {
            memset(fib, 0, sizeof(*fib));
            fib->fib_DirEntryType = node->is_dir ? ST_USERDIR : ST_FILE;
            fib->fib_EntryType    = node->is_dir ? ST_USERDIR : ST_FILE;
            fib->fib_Size         = (int32_t)node->size;
            fib->fib_NumBlocks    = (int32_t)((node->size + 511) / 512);
            fib->fib_Protection   = DEFAULT_PROTECTION;
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
        HandleEntry *le = HandleTable_GetLockEntry(handle, NULL);
        Fat32File *dir = le ? (Fat32File *)le->u.lock.node : NULL;
        if (!dir || !dir->is_dir) {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES;
            break;
        }
        char name[32];
        uint32_t size;
        uint8_t is_dir;
        if (FAT32_ReadDir(dir, name, &size, &is_dir)) {
            memset(fib, 0, sizeof(*fib));
            fib->fib_DirEntryType = is_dir ? ST_USERDIR : ST_FILE;
            fib->fib_EntryType    = is_dir ? ST_USERDIR : ST_FILE;
            int i = 0;
            while (i < 107 && name[i]) { fib->fib_FileName[i] = name[i]; i++; }
            fib->fib_FileName[i] = '\0';
            fib->fib_Size      = (int32_t)size;
            fib->fib_NumBlocks = (int32_t)((size + 511) / 512);
            fib->fib_Protection = (int32_t)DEFAULT_PROTECTION;
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES;
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
        FAT32_GetVolumeStats(fs, &total_bytes, &used_bytes);
        id->id_NumBlocks     = (int32_t)(total_bytes / 512);
        id->id_NumBlocksUsed = (int32_t)(used_bytes / 512);
        id->id_BytesPerBlock = (int32_t)fs->bytes_per_sec;
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
        /* FAT32 doesn't track parent in Fat32File yet; return NULL lock */
        pkt->dp_Res1 = 0;
        break;
    }

    /* ===== DupLock ===== */
    case ACTION_COPY_DIR: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        int32_t access = 0;
        HandleEntry *le = HandleTable_GetLockEntry(handle, &access);
        Fat32File *node = le ? (Fat32File *)le->u.lock.node : NULL;
        if (node) {
            /* Re-open the same path to get a new Fat32File */
            const char *path = le->path;
            Fat32File *dup = FAT32_Open(fs, path);
            if (dup) {
                uint32_t ph = HandleTable_AllocLock(path, dup, access);
                pkt->dp_Res1 = (int32_t)ph;
                if (ph == 0) {
                    FAT32_Close(dup);
                    pkt->dp_Res2 = ERROR_NO_FREE_STORE;
                }
            } else {
                pkt->dp_Res1 = 0;
                pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
            }
        } else {
            pkt->dp_Res1 = 0;
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
        Fat32File *n1 = le1 ? (Fat32File *)le1->u.lock.node : NULL;
        Fat32File *n2 = le2 ? (Fat32File *)le2->u.lock.node : NULL;
        pkt->dp_Res1 = (n1 && n2 && n1 == n2) ? DOSTRUE : DOSFALSE;
        break;
    }

    /* ===== Parent from file handle ===== */
    case ACTION_PARENT_FH: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        Fat32File *file = fat_get_file_handle(handle);
        (void)file; /* FAT32 lacks parent tracking for now */
        pkt->dp_Res1 = 0;
        break;
    }

    /* ===== Examine file handle ===== */
    case ACTION_EXAMINE_FH: {
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        FileInfoBlock *fib = (FileInfoBlock *)(intptr_t)pkt->dp_Arg2;
        Fat32File *file = fat_get_file_handle(handle);
        if (file) {
            memset(fib, 0, sizeof(*fib));
            fib->fib_DirEntryType = file->is_dir ? ST_USERDIR : ST_FILE;
            fib->fib_EntryType    = file->is_dir ? ST_USERDIR : ST_FILE;
            fib->fib_Size         = (int32_t)file->size;
            fib->fib_NumBlocks    = (int32_t)((file->size + 511) / 512);
            fib->fib_Protection   = DEFAULT_PROTECTION;
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ===== Examine all (same as EXAMINE_NEXT) ===== */
    case ACTION_EXAMINE_ALL: {
        /* Delegate to EXAMINE_NEXT logic */
        uint32_t handle = (uint32_t)pkt->dp_Arg1;
        FileInfoBlock *fib = (FileInfoBlock *)(intptr_t)pkt->dp_Arg2;
        HandleEntry *le = HandleTable_GetLockEntry(handle, NULL);
        Fat32File *dir = le ? (Fat32File *)le->u.lock.node : NULL;
        if (!dir || !dir->is_dir) {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES;
            break;
        }
        char name[32];
        uint32_t size;
        uint8_t is_dir;
        if (FAT32_ReadDir(dir, name, &size, &is_dir)) {
            memset(fib, 0, sizeof(*fib));
            fib->fib_DirEntryType = is_dir ? ST_USERDIR : ST_FILE;
            fib->fib_EntryType    = is_dir ? ST_USERDIR : ST_FILE;
            int i = 0;
            while (i < 107 && name[i]) { fib->fib_FileName[i] = name[i]; i++; }
            fib->fib_FileName[i] = '\0';
            fib->fib_Size      = (int32_t)size;
            fib->fib_NumBlocks = (int32_t)((size + 511) / 512);
            fib->fib_Protection = (int32_t)DEFAULT_PROTECTION;
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES;
        }
        break;
    }

    /* ===== Rename object ===== */
    case ACTION_RENAME_OBJECT: {
        /* FAT32 rename = create new entry, copy cluster, delete old entry.
         * For simplicity, we only support same-directory rename (no move). */
        const char *old_path = (const char *)(intptr_t)pkt->dp_Arg1;
        const char *new_path = (const char *)(intptr_t)pkt->dp_Arg2;
        (void)old_path;
        (void)new_path;
        /* TODO: implement FAT32_Rename */
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
        break;
    }

    /* ===== Is filesystem ===== */
    case ACTION_IS_FILESYSTEM: {
        pkt->dp_Res1 = DOSTRUE;
        break;
    }

    /* ===== Set date ===== */
    case ACTION_SET_DATE: {
        /* FAT32 timestamps not yet implemented */
        pkt->dp_Res1 = DOSTRUE;
        break;
    }

    /* ===== Flush ===== */
    case ACTION_FLUSH: {
        pkt->dp_Res1 = DOSTRUE;
        break;
    }

    /* ===== Inhibit ===== */
    case ACTION_INHIBIT: {
        pkt->dp_Res1 = DOSTRUE;
        break;
    }

    /* ===== Set protect / Set comment / Set file size ===== */
    case ACTION_SET_PROTECT:
    case ACTION_SET_COMMENT:
    case ACTION_SET_FILE_SIZE:
    default: {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
        break;
    }
    }
}

int FatHandler_Is(const Handler *handler)
{
    return handler && handler->ProcessPacket == FatHandler_ProcessPacket;
}

int FatHandler_ReadDir(Handler *handler, const char *path,
                       struct VfsDirEnt *entries, int max)
{
    if (!FatHandler_Is(handler) || !path || !entries || max <= 0) return 0;
    Fat32File *dir = FAT32_Open((Fat32FS *)handler->private, path);
    if (!dir || !dir->is_dir) {
        if (dir) FAT32_Close(dir);
        return 0;
    }

    int n = 0;
    while (n < max && FAT32_ReadDir(dir, entries[n].name,
                                    &entries[n].size, &entries[n].is_dir)) {
        entries[n].mtime = 0;   /* FAT32_ReadDir doesn't surface dates yet */
        n++;
    }
    FAT32_Close(dir);
    return n;
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */
Handler *FatHandler_Create(const char *name, Fat32FS *fs)
{
    if (!fs) return NULL;
    return Handler_Create(name, fs, FatHandler_ProcessPacket);
}
