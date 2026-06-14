/* fat_handler.c — AmigaDOS packet handler for FAT32 block devices
 *
 * Wraps the existing FAT32 driver in the Handler/DoPkt model.
 * This is a skeleton: the underlying FAT32 file operations (Open,
 * ReadDir, etc.) are currently stubs in fat32.c.  The architecture is
 * fully wired, so once those stubs are implemented this handler will
 * work without further changes.
 */

#include "fat_handler.h"
#include "dos/dospacket.h"
#include "dos/amiga_dos_types.h"
#include "dos/handle_table.h"
#include <stddef.h>
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
        const char *path = (const char *)pkt->dp_Arg1;
        Fat32File *file = FAT32_Open(fs, path);
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
        void *buf = (void *)pkt->dp_Arg2;
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
        const void *buf = (const void *)pkt->dp_Arg2;
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
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
        break;
    }

    /* ===== Create directory ===== */
    case ACTION_CREATE_DIR: {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
        break;
    }

    /* ===== Lock / Locate object ===== */
    case ACTION_LOCATE_OBJECT: {
        const char *path = (const char *)pkt->dp_Arg1;
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
        FileInfoBlock *fib = (FileInfoBlock *)pkt->dp_Arg2;
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

    /* ===== Examine next ===== */
    case ACTION_EXAMINE_NEXT: {
        /* TODO: implement directory iteration once FAT32_ReadDir is functional */
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES;
        break;
    }

    /* ===== Disk info ===== */
    case ACTION_DISK_INFO:
    case ACTION_INFO: {
        InfoData *id = (InfoData *)pkt->dp_Arg2;
        if (!id) {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
            break;
        }
        /* TODO: Real FAT32 stats once FAT32_GetVolumeStats exists */
        id->id_NumBlocks     = 0;
        id->id_NumBlocksUsed = 0;
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
            /* FAT32 has no DupLock concept; allocate a new lock on same path */
            uint32_t ph = HandleTable_AllocLock("", node, access);
            pkt->dp_Res1 = (int32_t)ph;
            if (ph == 0) pkt->dp_Res2 = ERROR_NO_FREE_STORE;
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

    /* ===== Rename / Set protect / Set comment ===== */
    case ACTION_RENAME_OBJECT:
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

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */
Handler *FatHandler_Create(const char *name, Fat32FS *fs)
{
    if (!fs) return NULL;
    return Handler_Create(name, fs, FatHandler_ProcessPacket);
}
