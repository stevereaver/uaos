/* vfs.c — UAOS Virtual Filesystem Layer */

#include "vfs.h"
#include "ramfs.h"
#include "amiga_dos_types.h"
#include "ram_handler.h"
#include "fat_handler.h"
#include "handle_table.h"
#include "blockdev.h"
#include "boot/kprint.h"
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Mount table — maps "VOL" names to RamFsVol instances
 * ========================================================================= */

#define MAX_MOUNTS  16

typedef struct {
    char      vol_name[16];  /* e.g. "RAM" (no colon) — unit/mount name */
    char      vol_label[16]; /* filesystem volume label, e.g. FAT "WB" —
                              * resolvable alias + display name (empty = none) */
    RamFsVol *vol;          /* direct pointer for native VFS access */
    Handler  *handler;      /* packet handler for DoPkt routing */
} MountEntry;

static MountEntry g_mounts[MAX_MOUNTS];
static int        g_n_mounts = 0;

/* Bumped by VFS_NoteChange() on every directory-visible mutation — by the
 * VFS_* functions below for direct RAMFS calls, and by DoPkt() for all
 * handler-routed packets (which also covers guest dos.library calls). */
static uint32_t   g_vfs_change_seq = 0;

uint32_t VFS_ChangeSeq(void) { return g_vfs_change_seq; }
void     VFS_NoteChange(void) { g_vfs_change_seq++; }

/* =========================================================================
 * Helpers
 * ========================================================================= */

static int slen(const char *s) { int n=0; while(s[n]) n++; return n; }

static int seq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* Extract "VOL" from "VOL:path" into dst[max].  Returns length or 0. */
static int extract_vol(const char *path, char *dst, int max)
{
    int i = 0;
    while (path[i] && path[i] != ':' && i < max - 1) {
        dst[i] = path[i]; i++;
    }
    dst[i] = '\0';
    return (path[i] == ':') ? i : 0;
}

/* Case-insensitive string compare */
static int seq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
        if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
        if (ca != cb) return 0;
        a++; b++;
    }
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
    if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
    return ca == cb;
}

/* Find mount entry by name (case-insensitive) */
static MountEntry *find_mount(const char *name)
{
    /* First check if this is an assign */
    const char *assign_target = VFS_ResolveAssign(name);
    if (assign_target) {
        char target_vol[16];
        if (extract_vol(assign_target, target_vol, 16)) {
            for (int i = 0; i < g_n_mounts; i++)
                if (seq_ci(g_mounts[i].vol_name, target_vol))
                    return &g_mounts[i];
        }
        return NULL;
    }

    /* Direct volume lookup — unit names take precedence over labels so
     * a disk labeled "RAM" can't shadow the real RAM: mount. */
    for (int i = 0; i < g_n_mounts; i++)
        if (seq_ci(g_mounts[i].vol_name, name))
            return &g_mounts[i];
    /* AmigaDOS semantics: the filesystem's volume label is also a valid
     * path prefix ("cd wb:" on the disk whose unit is DH0:). */
    for (int i = 0; i < g_n_mounts; i++)
        if (g_mounts[i].vol_label[0] && seq_ci(g_mounts[i].vol_label, name))
            return &g_mounts[i];
    return NULL;
}

/* Find mounted volume by name (case-insensitive) */
static RamFsVol *find_vol(const char *name)
{
    MountEntry *m = find_mount(name);
    return m ? m->vol : NULL;
}

/* Find handler by name (case-insensitive) */
static Handler *find_handler(const char *name)
{
    MountEntry *m = find_mount(name);
    return m ? m->handler : NULL;
}

/* Get the actual target path for a path that may contain assigns.
 * Writes resolved path to dst[max]. Returns dst or NULL on error.
 * Chained assigns are fully expanded ("ACElib:x" -> "ACE:lib/x" ->
 * "SYS:ACE/lib/x" -> "Workbench:ACE/lib/x"), depth-capped to guard
 * against assign cycles. */
static const char *resolve_assign_path(const char *path, char *dst, int max)
{
    char src[128];
    int i = 0;
    while (i < (int)sizeof(src) - 1 && path[i]) { src[i] = path[i]; i++; }
    src[i] = '\0';

    for (int depth = 0; depth < 8; depth++) {
        char vol_name[16];
        int vl = extract_vol(src, vol_name, 16);
        if (!vl) break; /* No volume prefix */

        const char *assign_target = VFS_ResolveAssign(vol_name);
        if (!assign_target) break; /* Not an assign - resolved */

        /* Expand assign: "C:dir" -> "Workbench:C/dir" */
        int ti = 0;
        while (ti < max - 1 && assign_target[ti]) { dst[ti] = assign_target[ti]; ti++; }

        /* Append rest of path */
        const char *rest = src + vl + 1; /* skip "VOL:" */
        if (*rest) {
            if (ti < max - 1 && assign_target[ti-1] != ':') dst[ti++] = '/';
            while (ti < max - 1 && *rest) { dst[ti++] = *rest++; }
        }
        dst[ti] = '\0';

        /* Feed the expansion back in for the next hop */
        i = 0;
        while (i < (int)sizeof(src) - 1 && dst[i]) { src[i] = dst[i]; i++; }
        src[i] = '\0';
    }

    /* Return the last expansion (or the unexpanded path if no hops ran) */
    i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return dst;
}

/* Forward declaration — defined in the Assign Support section below */
static const char *expand_with_target(const char *path, int vol_len,
                                      const char *target, char *dst, int max);

/* Copy a filesystem volume label into a mount entry (truncated to 15). */
static void mount_set_label(MountEntry *m, const char *label)
{
    int i = 0;
    if (label)
        while (i < 15 && label[i]) { m->vol_label[i] = label[i]; i++; }
    m->vol_label[i] = '\0';
}

/* Register a mounted volume with an associated packet handler */
static void register_mount(const char *name, const char *label,
                           RamFsVol *vol, Handler *handler)
{
    if (g_n_mounts >= MAX_MOUNTS) return;
    int i = 0;
    while (i < 15 && name[i]) { g_mounts[g_n_mounts].vol_name[i] = name[i]; i++; }
    g_mounts[g_n_mounts].vol_name[i] = '\0';
    mount_set_label(&g_mounts[g_n_mounts], label);
    g_mounts[g_n_mounts].vol     = vol;
    g_mounts[g_n_mounts].handler = handler;
    g_n_mounts++;
}

/* =========================================================================
 * Init — mount RAM: and create standard directories
 * ========================================================================= */

void VFS_Init(void)
{
    RamFS_Init();
    HandleTable_Init();

    /* Mount RAM: */
    RamFsVol *ram = RamFS_MountVol("RAM");
    if (!ram) return;
    Handler *ram_handler = RamHandler_Create("ram-handler", ram);
    register_mount("RAM", NULL, ram, ram_handler);

    /* Standard AmigaDOS RAM disk directories */
    RamFS_MkDir(ram, "RAM:T");
    RamFS_MkDir(ram, "RAM:ENV");
    RamFS_MkDir(ram, "RAM:ENVARC");
    RamFS_MkDir(ram, "RAM:CLIPS");
    RamFS_MkDir(ram, "RAM:S");
    RamFS_MkDir(ram, "RAM:Trash");

    /* ENV: and ENVARC: assigns (AmigaOS semantics):
     * ENV:     = volatile runtime prefs (RAM:)
     * ENVARC:  = persistent prefs (would be on writable SYS: in real Amiga) */
    VFS_AddAssign("ENV", "RAM:ENV", 0, 0);
    VFS_AddAssign("ENVARC", "RAM:ENVARC", 0, 0);
}

/* Setup default Workbench assigns after Workbench: is mounted */
void VFS_SetupWorkbenchAssigns(void)
{
    /* Check if Workbench: is mounted (RAMFS or handler-backed) */
    if (!find_vol("Workbench") && !find_handler("Workbench")) {
        extern void kprint(const char *);
        kprint("[VFS] Workbench: not found, assigns not created\n");
        return;
    }

    extern void kprint(const char *);
    kprint("[VFS] Creating Workbench assigns...\n");

    /* Create standard AmigaDOS assigns pointing to Workbench subdirectories */
    if (VFS_AddAssign("C", "Workbench:C", 0, 0) == 0) kprint("[VFS]  C: -> Workbench:C\n");
    if (VFS_AddAssign("S", "Workbench:S", 0, 0) == 0) kprint("[VFS]  S: -> Workbench:S\n");
    if (VFS_AddAssign("L", "Workbench:L", 0, 0) == 0) kprint("[VFS]  L: -> Workbench:L\n");
    if (VFS_AddAssign("DEVS", "Workbench:DEVS", 0, 0) == 0) kprint("[VFS]  DEVS: -> Workbench:DEVS\n");
    if (VFS_AddAssign("LIBS", "Workbench:LIBS", 0, 0) == 0) kprint("[VFS]  LIBS: -> Workbench:LIBS\n");
    /* SYS: is the boot volume root (AmigaOS semantics) */
    if (VFS_AddAssign("SYS", "Workbench:", 0, 0) == 0) kprint("[VFS]  SYS: -> Workbench:\n");
    if (VFS_AddAssign("Tools", "Workbench:Tools", 0, 0) == 0) kprint("[VFS]  Tools: -> Workbench:Tools\n");
    if (VFS_AddAssign("Utilities", "Workbench:Utilities", 0, 1) == 0) kprint("[VFS]  Utilities: -> Workbench:Utilities (deferred)\n");
    if (VFS_AddAssign("Classes", "Workbench:Classes", 0, 1) == 0) kprint("[VFS]  Classes: -> Workbench:Classes (deferred)\n");
    /* FONTS: assign for bitmap font support */
    if (VFS_AddAssign("FONTS", "Workbench:Fonts", 0, 1) == 0) kprint("[VFS]  FONTS: -> Workbench:Fonts (deferred)\n");
}

/* =========================================================================
 * Partition volume mounting (FAT32 partitions backed by Handler/DoPkt).
 * Only mounts if the underlying block device contains a valid FAT32
 * filesystem.  No placeholder mount is created for empty/unrecognised
 * partitions, so the desktop doesn't get cluttered with ghost icons.
 * ========================================================================= */

/* Case-insensitive comparison stopping at ':' or '\0'. */
static int name_eq_ci_colon(const char *a, const char *b)
{
    while (*a && *a != ':' && *b && *b != ':') {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return (*a == '\0' || *a == ':') && (*b == '\0' || *b == ':');
}

/* Find a block device whose display_name or name matches "name" (with or
 * without trailing colon). */
static BlockDev *find_bdev_by_name(const char *name)
{
    BlockDev *dev = BlockDev_GetList();
    while (dev) {
        if (dev->display_name && name_eq_ci_colon(name, dev->display_name))
            return dev;
        if (dev->name && name_eq_ci_colon(name, dev->name))
            return dev;
        dev = dev->next;
    }
    return NULL;
}

int VFS_MountPartition(const char *name)
{
    if (!name || !*name) return -1;

    /* Check if already mounted */
    for (int i = 0; i < g_n_mounts; i++) {
        if (seq(g_mounts[i].vol_name, name))
            return 0;  /* already mounted */
    }

    if (g_n_mounts >= MAX_MOUNTS) return -1;

    /* Try to find the underlying block device and mount it as FAT32 */
    BlockDev *bdev = find_bdev_by_name(name);
    if (!bdev || !BlockDev_CheckFormatted(bdev))
        return -1;

    Fat32FS *fs = FAT32_Mount(bdev);
    if (!fs) return -1;

    char label[16];
    FAT32_VolumeLabel(fs, label, sizeof(label));

    Handler *handler = FatHandler_Create(name, fs);
    if (!handler) {
        FAT32_Unmount(fs);
        return -1;
    }

    register_mount(name, label[0] ? label : NULL, NULL, handler);
    return 0;
}

int VFS_RemountPartition(const char *name)
{
    if (!name || !*name) return -1;

    BlockDev *bdev = find_bdev_by_name(name);
    if (!bdev || !BlockDev_CheckFormatted(bdev)) return -1;

    Fat32FS *fs = FAT32_Mount(bdev);
    if (!fs) return -1;

    char label[16];
    FAT32_VolumeLabel(fs, label, sizeof(label));

    for (int i = 0; i < g_n_mounts; i++) {
        if (seq(g_mounts[i].vol_name, name)) {
            if (!g_mounts[i].handler) return -1;
            g_mounts[i].handler->private = fs;
            mount_set_label(&g_mounts[i], label);
            return 0;
        }
    }

    return VFS_MountPartition(name);
}

int VFS_MountExistingVol(const char *name, RamFsVol *vol)
{
    if (!name || !*name || !vol) return -1;

    /* Check if already mounted */
    for (int i = 0; i < g_n_mounts; i++) {
        if (seq(g_mounts[i].vol_name, name))
            return 0;  /* already mounted */
    }

    if (g_n_mounts >= MAX_MOUNTS) return -1;

    Handler *handler = RamHandler_Create(name, vol);
    register_mount(name, NULL, vol, handler);
    return 0;
}

int VFS_MountFat(const char *name, BlockDev *bdev)
{
    if (!name || !*name || !bdev) return -1;

    /* Check if already mounted */
    for (int i = 0; i < g_n_mounts; i++) {
        if (seq(g_mounts[i].vol_name, name))
            return 0;  /* already mounted */
    }

    if (g_n_mounts >= MAX_MOUNTS) return -1;

    Fat32FS *fs = FAT32_Mount(bdev);
    if (!fs) return -1;

    char label[16];
    FAT32_VolumeLabel(fs, label, sizeof(label));

    Handler *handler = FatHandler_Create(name, fs);
    if (!handler) {
        FAT32_Unmount(fs);
        return -1;
    }

    register_mount(name, label[0] ? label : NULL, NULL, handler);
    return 0;
}

int VFS_GetMountCount(void)
{
    return g_n_mounts;
}

int VFS_GetMountName(int idx, char *dst, int max)
{
    if (idx < 0 || idx >= g_n_mounts || !dst || max < 2) return 0;
    /* Display name is the filesystem volume label when present
     * (AmigaDOS: the desktop shows "Workbench", not "DH0"), else the
     * unit/mount name.  Both names resolve as path prefixes. */
    const char *name = g_mounts[idx].vol_label[0]
                       ? g_mounts[idx].vol_label
                       : g_mounts[idx].vol_name;
    int i = 0;
    while (i < max - 1 && name[i]) {
        dst[i] = name[i];
        i++;
    }
    dst[i] = '\0';
    return 1;
}

/* =========================================================================
 * VFS_Open
 * ========================================================================= */

/* Return 1 if path is "NIL:" (case-insensitive) */
static int is_nil(const char *path)
{
    if (!path) return 0;
    const char *p = path;
    while (*p == ' ') p++;
    if ((p[0] == 'N' || p[0] == 'n') &&
        (p[1] == 'I' || p[1] == 'i') &&
        (p[2] == 'L' || p[2] == 'l') &&
        p[3] == ':') return 1;
    return 0;
}

int VFS_Open(VfsFile *fh, const char *path, int flags)
{
    fh->node        = NULL;
    fh->pos         = 0;
    fh->nil         = 0;
    fh->handle_id   = 0;
    fh->handler_port= NULL;

    /* Check if this is a multi-assign path */
    char vol_name[16];
    int vl = extract_vol(path, vol_name, 16);

    int target_count = 0;
    if (vl) target_count = VFS_GetAssignTargetCount(vol_name);

    if (target_count > 1 && !(flags & VFS_CREATE)) {
        /* Multi-assign file search: try each target in order */
        char resolved_path[128];
        for (int t = 0; t < target_count; t++) {
            const char *target = VFS_GetAssignTarget(vol_name, t);
            if (!target) continue;
            expand_with_target(path, vl, target, resolved_path,
                               sizeof(resolved_path));
            /* Targets may themselves route through assigns */
            resolve_assign_path(resolved_path, resolved_path,
                                sizeof(resolved_path));

            char rvol[16];
            if (!extract_vol(resolved_path, rvol, 16)) continue;

            /* Try RAMFS first */
            RamFsVol *vol = find_vol(rvol);
            if (vol) {
                RamFsNode *node = RamFS_Resolve(vol, resolved_path);
                if (node && node->type == RAMFS_TYPE_FILE) {
                    if (flags & VFS_TRUNC) node->size = 0;
                    fh->node = node;
                    fh->pos = 0;
                    return 1;
                }
                continue;
            }

            /* Try handler-backed filesystem */
            Handler *h = find_handler(rvol);
            if (h) {
                int32_t action = (flags & VFS_WRITE)
                                 ? ACTION_FINDUPDATE : ACTION_FINDINPUT;
                int32_t res = DoPkt(&h->port, action,
                                    (intptr_t)resolved_path,
                                    0, 0, 0, 0);
                if (res != 0 && res != DOSFALSE) {
                    fh->handle_id    = (uint32_t)res;
                    fh->handler_port = &h->port;
                    fh->pos          = 0;
                    return 1;
                }
            }
        }
        /* Not found in any target */
        return 0;
    }

    /* Standard single-target resolution (also used for CREATE) */
    char resolved_path[128];
    if (!resolve_assign_path(path, resolved_path, sizeof(resolved_path))) return 0;

    if (is_nil(resolved_path)) {
        fh->nil = 1;
        return 1;
    }

    char rvol[16];
    if (!extract_vol(resolved_path, rvol, 16)) return 0;

    /* RAMFS-backed volume */
    RamFsVol *vol = find_vol(rvol);
    if (vol) {
        RamFsNode *node = RamFS_Resolve(vol, resolved_path);

        if (!node) {
            if (!(flags & VFS_CREATE)) return 0;
            node = RamFS_Create(vol, resolved_path);
            if (!node) return 0;
            g_vfs_change_seq++;
        } else {
            if (node->type == RAMFS_TYPE_DIR) return 0; /* can't open dir as file */
            if (flags & VFS_TRUNC) { node->size = 0; g_vfs_change_seq++; }
        }

        fh->node = node;
        fh->pos  = 0;
        return 1;
    }

    /* Handler-backed filesystem (FAT32, future FS types, etc.) */
    Handler *h = find_handler(rvol);
    if (h) {
        int32_t action;
        if (flags & (VFS_CREATE | VFS_TRUNC))
            action = ACTION_FINDOUTPUT;       /* create + truncate */
        else if (flags & VFS_WRITE)
            action = ACTION_FINDUPDATE;       /* read/write, create if missing */
        else
            action = ACTION_FINDINPUT;        /* read-only */

        int32_t res = DoPkt(&h->port, action,
                            (intptr_t)resolved_path,
                            0, 0, 0, 0);
        /* dp_Res1 is the handler file handle (non-zero, non-DOSFALSE on success) */
        if (res != 0 && res != DOSFALSE) {
            fh->handle_id    = (uint32_t)res;
            fh->handler_port = &h->port;
            fh->pos          = 0;
            return 1;
        }
        return 0;
    }

    return 0;
}

void VFS_Close(VfsFile *fh)
{
    /* If handler-backed, send ACTION_END to release the handler file handle */
    if (fh->handler_port && fh->handle_id) {
        DoPkt(fh->handler_port, ACTION_END,
              (int32_t)fh->handle_id, 0, 0, 0, 0);
    }
    fh->node        = NULL;
    fh->pos         = 0;
    fh->nil         = 0;
    fh->handle_id   = 0;
    fh->handler_port= NULL;
}

uint32_t VFS_Read(VfsFile *fh, uint8_t *buf, uint32_t len)
{
    if (fh->nil) return 0; /* EOF immediately */

    /* Handler-backed file: dispatch ACTION_READ */
    if (fh->handler_port && fh->handle_id) {
        int32_t got = DoPkt(fh->handler_port, ACTION_READ,
                            (int32_t)fh->handle_id,
                            (intptr_t)buf,
                            (int32_t)len, 0, 0);
        if (got > 0) fh->pos += (uint32_t)got;
        return (got > 0) ? (uint32_t)got : 0;
    }

    if (!fh->node) return 0;
    uint32_t got = RamFS_Read(fh->node, fh->pos, buf, len);
    fh->pos += got;
    return got;
}

/* Block size pre-allocated per file on first write.
 * 4 KB covers typical shell output, env vars, and small scripts while
 * keeping the bump-allocator pool from being exhausted too quickly. */
#define VFS_BLOCK_SZ  (4 * 1024)

uint32_t VFS_Write(VfsFile *fh, const uint8_t *buf, uint32_t len)
{
    if (fh->nil) return len; /* discard silently */

    /* Handler-backed file: dispatch ACTION_WRITE */
    if (fh->handler_port && fh->handle_id) {
        int32_t wrote = DoPkt(fh->handler_port, ACTION_WRITE,
                              (int32_t)fh->handle_id,
                              (intptr_t)buf,
                              (int32_t)len, 0, 0);
        if (wrote > 0) fh->pos += (uint32_t)wrote;
        return (wrote > 0) ? (uint32_t)wrote : 0;
    }

    if (!fh->node || fh->node->type != RAMFS_TYPE_FILE) return 0;

    uint32_t end = fh->pos + len;
    if (end > RAMFS_MAX_FILESIZE) {
        len = RAMFS_MAX_FILESIZE - fh->pos;
        end = RAMFS_MAX_FILESIZE;
    }
    if (len == 0) return 0;

    /* First write to this node: allocate a full block from the pool */
    if (fh->node->alloc == 0) {
        uint32_t alloc_sz = end < VFS_BLOCK_SZ ? VFS_BLOCK_SZ : end;
        if (alloc_sz > RAMFS_MAX_FILESIZE) alloc_sz = RAMFS_MAX_FILESIZE;
        uint8_t *pool_buf = RamFS_AllocPool(alloc_sz);
        if (!pool_buf) return 0;
        fh->node->data  = pool_buf;
        fh->node->alloc = alloc_sz;
        fh->node->size  = 0;
    }

    /* Allocation too small — can't grow (bump allocator), truncate write */
    if (end > fh->node->alloc) {
        len = fh->node->alloc - fh->pos;
        end = fh->node->alloc;
        if (len == 0) return 0;
    }

    for (uint32_t i = 0; i < len; i++)
        fh->node->data[fh->pos + i] = buf[i];
    fh->pos += len;
    if (fh->pos > fh->node->size)
        fh->node->size = fh->pos;
    return len;
}

void VFS_Seek(VfsFile *fh, uint32_t pos)
{
    if (fh->nil) return;

    /* Handler-backed file: dispatch ACTION_SEEK */
    if (fh->handler_port && fh->handle_id) {
        DoPkt(fh->handler_port, ACTION_SEEK,
              (int32_t)fh->handle_id,
              (int32_t)pos, OFFSET_BEGINNING, 0, 0);
        fh->pos = pos;
        return;
    }

    if (!fh->node) return;
    fh->pos = (pos <= fh->node->size) ? pos : fh->node->size;
}

uint32_t VFS_Size(VfsFile *fh)
{
    if (fh->nil) return 0;

    /* Handler-backed file: use ACTION_SEEK with OFFSET_END to get size */
    if (fh->handler_port && fh->handle_id) {
        /* AmigaDOS Seek returns the position *before* the move, so
         * Seek(0, OFFSET_END) reports the old position, not the size.
         * A second Seek(0, OFFSET_CURRENT) is needed to read back the
         * end position (= file size), then restore the original pos. */
        int32_t old = DoPkt(fh->handler_port, ACTION_SEEK,
                            (int32_t)fh->handle_id,
                            0, OFFSET_END, 0, 0);
        int32_t size = DoPkt(fh->handler_port, ACTION_SEEK,
                             (int32_t)fh->handle_id,
                             0, OFFSET_CURRENT, 0, 0);
        DoPkt(fh->handler_port, ACTION_SEEK,
              (int32_t)fh->handle_id,
              old, OFFSET_BEGINNING, 0, 0);
        return (size > 0) ? (uint32_t)size : 0;
    }

    if (!fh->node) return 0;
    return fh->node->size;
}

int VFS_MkDir(const char *path)
{
    char resolved_path[128];
    if (!resolve_assign_path(path, resolved_path, sizeof(resolved_path))) return -1;

    char vol_name[16];
    if (!extract_vol(resolved_path, vol_name, 16)) return -1;

    /* RAMFS-backed volume */
    RamFsVol *vol = find_vol(vol_name);
    if (vol) {
        RamFsNode *node = RamFS_MkDir(vol, resolved_path);
        if (!node) return -1;
        g_vfs_change_seq++;
        return 0;
    }

    /* Handler-backed filesystem (FAT32, etc.) */
    Handler *h = find_handler(vol_name);
    if (h) {
        int32_t lock = DoPkt(&h->port, ACTION_CREATE_DIR,
                             (intptr_t)resolved_path, 0, 0, 0, 0);
        if (lock == 0 || lock == DOSFALSE) return -1;
        DoPkt(&h->port, ACTION_FREE_LOCK, lock, 0, 0, 0, 0);
        return 0;
    }

    return -1;
}

int VFS_Delete(const char *path)
{
    char resolved_path[128];
    if (!resolve_assign_path(path, resolved_path, sizeof(resolved_path))) return -1;

    char vol_name[16];
    if (!extract_vol(resolved_path, vol_name, 16)) return -1;

    /* RAMFS-backed volume */
    RamFsVol *vol = find_vol(vol_name);
    if (vol) {
        int res = RamFS_Delete(vol, resolved_path);
        if (res == 0) g_vfs_change_seq++;
        return res;
    }

    /* Handler-backed filesystem (FAT32, etc.) */
    Handler *h = find_handler(vol_name);
    if (h) {
        int32_t res = DoPkt(&h->port, ACTION_DELETE_OBJECT,
                            (intptr_t)resolved_path, 0, 0, 0, 0);
        return (res == DOSTRUE) ? 0 : -1;
    }

    return -1;
}

RamFsNode *VFS_OpenDir(const char *path)
{
    /* Check if this is a multi-assign path */
    char vol_name[16];
    int vl = extract_vol(path, vol_name, 16);

    int target_count = 0;
    if (vl) target_count = VFS_GetAssignTargetCount(vol_name);

    if (target_count > 1) {
        /* Multi-assign: return the first existing directory */
        char resolved_path[128];
        for (int t = 0; t < target_count; t++) {
            const char *target = VFS_GetAssignTarget(vol_name, t);
            if (!target) continue;
            expand_with_target(path, vl, target, resolved_path,
                               sizeof(resolved_path));
            /* Targets may themselves route through assigns */
            resolve_assign_path(resolved_path, resolved_path,
                                sizeof(resolved_path));

            char rvol[16];
            int rvl = extract_vol(resolved_path, rvol, 16);
            if (!rvl) continue;
            RamFsVol *vol = find_vol(rvol);
            if (!vol) continue;

            const char *after = resolved_path + rvl + 1;
            while (*after == '/') after++;
            if (*after == '\0') {
                return vol->root ? vol->root->first_child : NULL;
            }

            RamFsNode *node = RamFS_Resolve(vol, resolved_path);
            if (node && node->type == RAMFS_TYPE_DIR)
                return node->first_child;
        }
        return NULL;
    }

    /* Standard single-target resolution */
    char resolved_path[128];
    if (!resolve_assign_path(path, resolved_path, sizeof(resolved_path))) return NULL;

    char rvol[16];
    int rvl = extract_vol(resolved_path, rvol, 16);
    if (!rvl) return NULL;

    RamFsVol *vol = find_vol(rvol);
    if (!vol) return NULL;

    /* Handle bare volume root like "Workbench:" */
    const char *after = resolved_path + rvl + 1;
    while (*after == '/') after++;
    if (*after == '\0') {
        return vol->root ? vol->root->first_child : NULL;
    }

    RamFsNode *node = RamFS_Resolve(vol, resolved_path);
    if (!node || node->type != RAMFS_TYPE_DIR) return NULL;
    return node->first_child;
}

RamFsNode *VFS_ResolveDir(const char *path)
{
    /* Check if this is a multi-assign path */
    char vol_name[16];
    int vl = extract_vol(path, vol_name, 16);

    int target_count = 0;
    if (vl) target_count = VFS_GetAssignTargetCount(vol_name);

    if (target_count > 1) {
        /* Multi-assign: return the first existing directory */
        char resolved_path[128];
        for (int t = 0; t < target_count; t++) {
            const char *target = VFS_GetAssignTarget(vol_name, t);
            if (!target) continue;
            expand_with_target(path, vl, target, resolved_path,
                               sizeof(resolved_path));
            /* Targets may themselves route through assigns */
            resolve_assign_path(resolved_path, resolved_path,
                                sizeof(resolved_path));

            char rvol[16];
            int rvl = extract_vol(resolved_path, rvol, 16);
            if (!rvl) continue;
            RamFsVol *vol = find_vol(rvol);
            if (!vol) continue;

            const char *after = resolved_path + rvl + 1;
            while (*after == '/') after++;
            if (*after == '\0') {
                return vol->root;
            }

            RamFsNode *node = RamFS_Resolve(vol, resolved_path);
            if (node && node->type == RAMFS_TYPE_DIR)
                return node;
        }
        return NULL;
    }

    /* Standard single-target resolution */
    char resolved_path[128];
    if (!resolve_assign_path(path, resolved_path, sizeof(resolved_path))) return NULL;

    char rvol[16];
    int rvl = extract_vol(resolved_path, rvol, 16);

    /* Handle bare volume root like "RAM:" */
    if (rvl > 0) {
        const char *after = resolved_path + rvl + 1;
        while (*after == '/') after++;
        if (*after == '\0') {
            RamFsVol *vol = find_vol(rvol);
            return vol ? vol->root : NULL;
        }
    }

    RamFsVol *vol = find_vol(rvol);
    if (!vol) return NULL;
    RamFsNode *node = RamFS_Resolve(vol, resolved_path);
    if (!node || node->type != RAMFS_TYPE_DIR) return NULL;
    return node;
}

/* Returns 1 if path exists and is a directory (or a bare volume root),
 * 0 otherwise. Works for both RAMFS and handler-backed volumes. */
int VFS_IsDir(const char *path)
{
    char resolved_path[128];
    if (!resolve_assign_path(path, resolved_path, sizeof(resolved_path))) return 0;
    char rvol[16];
    int rvl = extract_vol(resolved_path, rvol, 16);
    if (rvl <= 0) return 0;

    /* Bare volume root like "DH0:" is always a directory if the
     * volume is mounted (RAMFS or handler-backed). */
    const char *after = resolved_path + rvl + 1;
    while (*after == '/') after++;
    if (*after == '\0') {
        return (find_vol(rvol) || find_handler(rvol)) ? 1 : 0;
    }

    /* RAMFS path */
    RamFsVol *vol = find_vol(rvol);
    if (vol) {
        RamFsNode *node = RamFS_Resolve(vol, resolved_path);
        return (node && node->type == RAMFS_TYPE_DIR) ? 1 : 0;
    }

    /* Handler-backed path (FAT32) */
    Handler *h = find_handler(rvol);
    if (h) {
        int32_t lock = DoPkt(&h->port, ACTION_LOCATE_OBJECT,
                              (intptr_t)resolved_path,
                              SHARED_LOCK, 0, 0, 0);
        if (lock == 0 || lock == DOSFALSE) return 0;
        FileInfoBlock fib;
        int32_t res = DoPkt(&h->port, ACTION_EXAMINE_OBJECT,
                            lock, (intptr_t)&fib, 0, 0, 0);
        DoPkt(&h->port, ACTION_FREE_LOCK, lock, 0, 0, 0, 0);
        if (res == DOSTRUE &&
            (fib.fib_DirEntryType == ST_USERDIR ||
             fib.fib_DirEntryType == ST_ROOTDIR)) {
            return 1;
        }
    }

    return 0;
}

RamFsNode *VFS_GetRoot(const char *vol_name)
{
    RamFsVol *vol = find_vol(vol_name);
    if (!vol) return NULL;
    return vol->root;
}

/* Generic directory reader: fills up to max VfsDirEnt structures.
 * Works for both RAMFS and handler-backed (FAT32) volumes.
 * Returns the number of entries read, or 0 on error/empty. */
int VFS_ReadDir(const char *path, VfsDirEnt *ents, int max)
{
    if (!ents || max <= 0) return 0;

    char resolved_path[128];
    if (!resolve_assign_path(path, resolved_path, sizeof(resolved_path))) return 0;
    char rvol[16];
    if (!extract_vol(resolved_path, rvol, 16)) return 0;

    /* RAMFS path */
    RamFsVol *vol = find_vol(rvol);
    if (vol) {
        RamFsNode *dir = VFS_ResolveDir(resolved_path);
        if (!dir) return 0;
        RamFsNode *child = dir->first_child;
        int n = 0;
        while (child && n < max) {
            int i = 0;
            while (i < RAMFS_MAX_NAME - 1 && child->name[i]) {
                ents[n].name[i] = child->name[i]; i++;
            }
            ents[n].name[i] = '\0';
            ents[n].is_dir = (child->type == RAMFS_TYPE_DIR) ? 1 : 0;
            ents[n].size = child->size;
            ents[n].mtime = child->mtime;
            n++;
            child = child->next_sibling;
        }
        return n;
    }

    /* Handler-backed path (FAT32) */
    Handler *h = find_handler(rvol);
    if (h && FatHandler_Is(h))
        return FatHandler_ReadDir(h, resolved_path, ents, max);
    if (h) {
        int32_t lock = DoPkt(&h->port, ACTION_LOCATE_OBJECT,
                              (intptr_t)resolved_path,
                              SHARED_LOCK, 0, 0, 0);
        if (lock == 0 || lock == DOSFALSE) return 0;

        /* Verify it is actually a directory */
        FileInfoBlock fib;
        int32_t res = DoPkt(&h->port, ACTION_EXAMINE_OBJECT,
                            lock, (intptr_t)&fib, 0, 0, 0);
        if (res != DOSTRUE || fib.fib_DirEntryType != ST_USERDIR) {
            DoPkt(&h->port, ACTION_FREE_LOCK, lock, 0, 0, 0, 0);
            return 0;
        }

        int n = 0;
        while (n < max) {
            res = DoPkt(&h->port, ACTION_EXAMINE_NEXT,
                        lock, (intptr_t)&fib, 0, 0, 0);
            if (res != DOSTRUE) break;
            int i = 0;
            while (i < RAMFS_MAX_NAME - 1 && fib.fib_FileName[i]) {
                ents[n].name[i] = fib.fib_FileName[i]; i++;
            }
            ents[n].name[i] = '\0';
            ents[n].is_dir = (fib.fib_DirEntryType == ST_USERDIR) ? 1 : 0;
            ents[n].size = (uint32_t)fib.fib_Size;
            /* Amiga DateStamp -> Unix epoch (same convention as
             * locale_lib.c: ds_Days is unix_days + 2922). Handlers that
             * don't fill fib_Date leave it zeroed -> mtime 0. */
            ents[n].mtime = (fib.fib_Date.ds_Days > 2922)
                ? (uint32_t)(fib.fib_Date.ds_Days - 2922) * 86400
                  + (uint32_t)fib.fib_Date.ds_Minute * 60
                  + (uint32_t)fib.fib_Date.ds_Tick / 50
                : 0;
            n++;
        }
        DoPkt(&h->port, ACTION_FREE_LOCK, lock, 0, 0, 0, 0);
        return n;
    }

    return 0;
}

uint8_t VFS_GetAttrs(const char *path)
{
    char resolved_path[128];
    if (!resolve_assign_path(path, resolved_path, sizeof(resolved_path))) return 0;

    char vol_name[16];
    if (!extract_vol(resolved_path, vol_name, 16)) return 0;
    RamFsVol *vol = find_vol(vol_name);
    if (vol) {
        RamFsNode *node = RamFS_Resolve(vol, resolved_path);
        if (!node) return 0;
        return RamFS_GetAttrs(node);
    }
    /* Handler-backed: no direct attr mapping yet */
    return 0;
}

int VFS_SetAttrs(const char *path, uint8_t attrs)
{
    char resolved_path[128];
    if (!resolve_assign_path(path, resolved_path, sizeof(resolved_path))) return -1;

    char vol_name[16];
    if (!extract_vol(resolved_path, vol_name, 16)) return -1;
    RamFsVol *vol = find_vol(vol_name);
    if (vol) {
        RamFsNode *node = RamFS_Resolve(vol, resolved_path);
        if (!node) return -1;
        return RamFS_SetAttrs(node, attrs);
    }
    /* Handler-backed: no direct attr mapping yet */
    return -1;
}

uint16_t VFS_GetProtection(const char *path)
{
    char resolved_path[128];
    if (!resolve_assign_path(path, resolved_path, sizeof(resolved_path))) return DEFAULT_PROTECTION;

    char vol_name[16];
    if (!extract_vol(resolved_path, vol_name, 16)) return DEFAULT_PROTECTION;
    RamFsVol *vol = find_vol(vol_name);
    if (vol) {
        RamFsNode *node = RamFS_Resolve(vol, resolved_path);
        if (!node) return DEFAULT_PROTECTION;
        return RamFS_GetProtection(node);
    }
    /* Handler-backed: use ACTION_LOCATE_OBJECT + ACTION_EXAMINE_OBJECT */
    Handler *h = find_handler(vol_name);
    if (h) {
        int32_t lock = DoPkt(&h->port, ACTION_LOCATE_OBJECT,
                              (intptr_t)resolved_path,
                              SHARED_LOCK, 0, 0, 0);
        if (lock == 0 || lock == DOSFALSE) return DEFAULT_PROTECTION;
        FileInfoBlock fib;
        int32_t res = DoPkt(&h->port, ACTION_EXAMINE_OBJECT,
                            lock, (intptr_t)&fib, 0, 0, 0);
        DoPkt(&h->port, ACTION_FREE_LOCK, lock, 0, 0, 0, 0);
        if (res == DOSTRUE)
            return (uint16_t)fib.fib_Protection;
    }
    return DEFAULT_PROTECTION;
}

int VFS_SetProtection(const char *path, uint16_t prot)
{
    char resolved_path[128];
    if (!resolve_assign_path(path, resolved_path, sizeof(resolved_path))) return -1;

    char vol_name[16];
    if (!extract_vol(resolved_path, vol_name, 16)) return -1;
    RamFsVol *vol = find_vol(vol_name);
    if (vol) {
        RamFsNode *node = RamFS_Resolve(vol, resolved_path);
        if (!node) return -1;
        return RamFS_SetProtection(node, prot);
    }
    /* Handler-backed: dispatch ACTION_SET_PROTECT */
    Handler *h = find_handler(vol_name);
    if (h) {
        int32_t res = DoPkt(&h->port, ACTION_SET_PROTECT,
                            (intptr_t)resolved_path,
                            (int32_t)prot, 0, 0, 0);
        return (res == DOSTRUE) ? 0 : -1;
    }
    return -1;
}

int VFS_GetComment(const char *path, char *dst, int max)
{
    if (!dst || max < 1) return -1;
    dst[0] = '\0';
    char resolved_path[128];
    if (!resolve_assign_path(path, resolved_path, sizeof(resolved_path))) return -1;
    char vol_name[16];
    if (!extract_vol(resolved_path, vol_name, 16)) return -1;
    RamFsVol *vol = find_vol(vol_name);
    if (vol) {
        RamFsNode *node = RamFS_Resolve(vol, resolved_path);
        if (!node) return -1;
        int i = 0;
        while (i < max - 1 && node->comment[i]) { dst[i] = node->comment[i]; i++; }
        dst[i] = '\0';
        return 0;
    }
    /* Handler-backed: use ACTION_LOCATE_OBJECT + ACTION_EXAMINE_OBJECT */
    Handler *h = find_handler(vol_name);
    if (h) {
        int32_t lock = DoPkt(&h->port, ACTION_LOCATE_OBJECT,
                              (intptr_t)resolved_path,
                              SHARED_LOCK, 0, 0, 0);
        if (lock == 0 || lock == DOSFALSE) return -1;
        FileInfoBlock fib;
        int32_t res = DoPkt(&h->port, ACTION_EXAMINE_OBJECT,
                            lock, (intptr_t)&fib, 0, 0, 0);
        DoPkt(&h->port, ACTION_FREE_LOCK, lock, 0, 0, 0, 0);
        if (res == DOSTRUE) {
            int i = 0;
            while (i < max - 1 && i < 79 && fib.fib_Comment[i]) {
                dst[i] = fib.fib_Comment[i]; i++;
            }
            dst[i] = '\0';
            return 0;
        }
    }
    return -1;
}

int VFS_SetComment(const char *path, const char *comment)
{
    char resolved_path[128];
    if (!resolve_assign_path(path, resolved_path, sizeof(resolved_path))) return -1;
    char vol_name[16];
    if (!extract_vol(resolved_path, vol_name, 16)) return -1;
    RamFsVol *vol = find_vol(vol_name);
    if (vol) {
        RamFsNode *node = RamFS_Resolve(vol, resolved_path);
        if (!node) return -1;
        int i = 0;
        while (i < 63 && comment && comment[i]) { node->comment[i] = comment[i]; i++; }
        node->comment[i] = '\0';
        return 0;
    }
    /* Handler-backed: dispatch ACTION_SET_COMMENT */
    Handler *h = find_handler(vol_name);
    if (h) {
        int32_t res = DoPkt(&h->port, ACTION_SET_COMMENT,
                            (intptr_t)resolved_path,
                            (intptr_t)comment, 0, 0, 0);
        return (res == DOSTRUE) ? 0 : -1;
    }
    return -1;
}

int VFS_RenameVol(const char *old_name, const char *new_name)
{
    if (!old_name || !*old_name || !new_name || !*new_name) return -1;
    RamFsVol *vol = find_vol(old_name);
    if (!vol) return -1;
    int res = RamFS_RenameVol(vol, new_name);
    if (res == 0) g_vfs_change_seq++;
    return res;
}

int VFS_Rename(const char *old_path, const char *new_path)
{
    if (!old_path || !*old_path || !new_path || !*new_path) return -1;

    char old_res[128], new_res[128];
    if (!resolve_assign_path(old_path, old_res, sizeof(old_res))) return -1;
    if (!resolve_assign_path(new_path, new_res, sizeof(new_res))) return -1;

    char old_vol[16], new_vol[16];
    if (!extract_vol(old_res, old_vol, 16)) return -1;
    if (!extract_vol(new_res, new_vol, 16)) return -1;

    /* Same-volume rename only */
    if (!seq_ci(old_vol, new_vol)) return -1;

    RamFsVol *vol = find_vol(old_vol);
    if (vol) {
        int res = RamFS_Rename(vol, old_res, new_res);
        if (res == 0) g_vfs_change_seq++;
        return res;
    }

    /* Handler-backed: dispatch ACTION_RENAME_OBJECT */
    Handler *h = find_handler(old_vol);
    if (h) {
        int32_t res = DoPkt(&h->port, ACTION_RENAME_OBJECT,
                            (intptr_t)old_res,
                            (intptr_t)new_res, 0, 0, 0);
        return (res == DOSTRUE) ? 0 : -1;
    }
    return -1;
}

int VFS_GetVolumeInfo(const char *path, uint32_t *total_bytes, uint32_t *used_bytes)
{
    if (!path || !*path || !total_bytes || !used_bytes) return -1;

    char resolved_path[128];
    if (!resolve_assign_path(path, resolved_path, sizeof(resolved_path))) return -1;
    char vol_name[16];
    if (!extract_vol(resolved_path, vol_name, 16)) return -1;

    RamFsVol *vol = find_vol(vol_name);
    if (vol) {
        RamFS_GetVolumeStats(vol, total_bytes, used_bytes);
        return 0;
    }

    /* Handler-backed: dispatch ACTION_DISK_INFO.  The InfoData pointer
     * goes in dp_Arg2 (dp_Arg1 is the volume lock, unused here). */
    Handler *h = find_handler(vol_name);
    if (h) {
        InfoData id;
        int32_t res = DoPkt(&h->port, ACTION_DISK_INFO,
                            0, (intptr_t)&id, 0, 0, 0);
        if (res == DOSTRUE) {
            *total_bytes = (uint32_t)id.id_NumBlocks * (uint32_t)id.id_BytesPerBlock;
            *used_bytes  = (uint32_t)id.id_NumBlocksUsed * (uint32_t)id.id_BytesPerBlock;
            return 0;
        }
    }
    return -1;
}

/* =========================================================================
 * AmigaDOS Assign Support
 * Assigns create logical names that map to physical paths.
 * Multi-assign (ADD) allows an assign to resolve to multiple directories.
 * Example: "Assign LIBS: Workbench:LIBS" then "Assign LIBS: SYS:Classes ADD"
 * makes "LIBS:foo" search Workbench:LIBS/foo then SYS:Classes/foo.
 * ========================================================================= */

#define MAX_ASSIGNS 64
#define ASSIGN_MAX_NAME 16
#define ASSIGN_MAX_PATH 64
#define ASSIGN_MAX_TARGETS 8

typedef struct {
    char name[ASSIGN_MAX_NAME];
    char targets[ASSIGN_MAX_TARGETS][ASSIGN_MAX_PATH];
    int  target_count;
    int  valid;
    int  deferred;
} AssignEntry;

static AssignEntry g_assigns[MAX_ASSIGNS];

/* Case-insensitive string compare (copy from above for static use) */
static int seq_ci_assign(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
        if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
        if (ca != cb) return 0;
        a++; b++;
    }
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
    if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
    return ca == cb;
}

static int slen_assign(const char *s) { int n=0; while(s[n]) n++; return n; }

/* Strip trailing colon from name if present */
static void strip_colon(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) {
        if (src[i] == ':') break;
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Find assign index by name, or -1 if not found */
static int find_assign_idx(const char *name)
{
    for (int i = 0; i < MAX_ASSIGNS; i++) {
        if (g_assigns[i].valid && seq_ci_assign(g_assigns[i].name, name))
            return i;
    }
    return -1;
}

/* Expand a path using a specific assign target (not looking up the assign).
 * path = "C:dir/file", vol_len = 1 (length of "C"), target = "Workbench:C"
 * result = "Workbench:C/dir/file" */
static const char *expand_with_target(const char *path, int vol_len,
                                      const char *target, char *dst, int max)
{
    int ti = 0;
    while (ti < max - 1 && target[ti]) { dst[ti] = target[ti]; ti++; }

    const char *rest = path + vol_len + 1; /* skip "VOL:" */
    if (*rest) {
        if (ti < max - 1 && target[ti - 1] != ':') dst[ti++] = '/';
        while (ti < max - 1 && *rest) { dst[ti++] = *rest++; }
    }
    dst[ti] = '\0';
    return dst;
}

int VFS_AddAssign(const char *assign_name, const char *target_path,
                  int add, int defer)
{
    if (!assign_name || !*assign_name || !target_path || !*target_path)
        return -1;

    /* Strip colon from assign name */
    char name[ASSIGN_MAX_NAME];
    strip_colon(name, assign_name, ASSIGN_MAX_NAME);
    if (!name[0]) return -1;

    /* Validate target path exists unless DEFER is set.
     * Resolve chained assigns first so targets like "ACE:lib"
     * (ACE -> SYS:ACE, SYS -> Workbench:) validate against the
     * real backing volume. */
    if (!defer) {
        char resolved[128];
        if (!resolve_assign_path(target_path, resolved, sizeof(resolved)))
            return -1;
        char test_vol[16];
        if (!extract_vol(resolved, test_vol, 16)) return -1;
        if (!find_vol(test_vol)) return -1;
    }

    int idx = find_assign_idx(name);

    if (idx >= 0 && add) {
        /* Append to existing multi-assign */
        if (g_assigns[idx].target_count >= ASSIGN_MAX_TARGETS) {
            /* Log to kernel console so '>NIL:' redirects cannot hide the drop */
            kprint("[VFS] WARN: assign '");
            kprint(name);
            kprint("' has max targets, dropping '");
            kprint(target_path);
            kprint("'\n");
            return -1; /* Too many targets */
        }
        /* Check for duplicate target */
        for (int t = 0; t < g_assigns[idx].target_count; t++) {
            if (seq_ci_assign(g_assigns[idx].targets[t], target_path))
                return 0; /* already present */
        }
        int ti = 0;
        while (ti < ASSIGN_MAX_PATH - 1 && target_path[ti]) {
            g_assigns[idx].targets[g_assigns[idx].target_count][ti] = target_path[ti];
            ti++;
        }
        g_assigns[idx].targets[g_assigns[idx].target_count][ti] = '\0';
        g_assigns[idx].target_count++;
        return 0;
    }

    if (idx >= 0 && !add) {
        /* Overwrite existing assign */
        g_assigns[idx].target_count = 1;
        int ti = 0;
        while (ti < ASSIGN_MAX_PATH - 1 && target_path[ti]) {
            g_assigns[idx].targets[0][ti] = target_path[ti];
            ti++;
        }
        g_assigns[idx].targets[0][ti] = '\0';
        g_assigns[idx].deferred = defer;
        return 0;
    }

    /* Find a free slot for a new assign */
    int free_idx = -1;
    for (int i = 0; i < MAX_ASSIGNS; i++) {
        if (!g_assigns[i].valid) {
            free_idx = i;
            break;
        }
    }
    if (free_idx < 0) {
        /* Log to kernel console so '>NIL:' redirects cannot hide the drop */
        kprint("[VFS] WARN: assign table full, dropping '");
        kprint(name);
        kprint("'\n");
        return -1; /* Table full */
    }

    int ni = 0;
    while (ni < ASSIGN_MAX_NAME - 1 && name[ni]) {
        g_assigns[free_idx].name[ni] = name[ni];
        ni++;
    }
    g_assigns[free_idx].name[ni] = '\0';

    int ti = 0;
    while (ti < ASSIGN_MAX_PATH - 1 && target_path[ti]) {
        g_assigns[free_idx].targets[0][ti] = target_path[ti];
        ti++;
    }
    g_assigns[free_idx].targets[0][ti] = '\0';
    g_assigns[free_idx].target_count = 1;
    g_assigns[free_idx].valid = 1;
    g_assigns[free_idx].deferred = defer;

    return 0;
}

int VFS_RemoveAssign(const char *assign_name)
{
    if (!assign_name || !*assign_name) return -1;

    char name[ASSIGN_MAX_NAME];
    strip_colon(name, assign_name, ASSIGN_MAX_NAME);
    if (!name[0]) return -1;

    int idx = find_assign_idx(name);
    if (idx < 0) return -1;

    g_assigns[idx].valid = 0;
    g_assigns[idx].name[0] = '\0';
    for (int t = 0; t < ASSIGN_MAX_TARGETS; t++)
        g_assigns[idx].targets[t][0] = '\0';
    g_assigns[idx].target_count = 0;
    g_assigns[idx].deferred = 0;
    return 0;
}

const char *VFS_ResolveAssign(const char *assign_name)
{
    if (!assign_name || !*assign_name) return NULL;

    char name[ASSIGN_MAX_NAME];
    strip_colon(name, assign_name, ASSIGN_MAX_NAME);
    if (!name[0]) return NULL;

    int idx = find_assign_idx(name);
    if (idx < 0) return NULL;
    return g_assigns[idx].targets[0];
}

int VFS_GetAssignTargetCount(const char *assign_name)
{
    if (!assign_name || !*assign_name) return 0;

    char name[ASSIGN_MAX_NAME];
    strip_colon(name, assign_name, ASSIGN_MAX_NAME);
    if (!name[0]) return 0;

    int idx = find_assign_idx(name);
    if (idx < 0) return 0;
    return g_assigns[idx].target_count;
}

const char *VFS_GetAssignTarget(const char *assign_name, int idx)
{
    if (!assign_name || !*assign_name || idx < 0) return NULL;

    char name[ASSIGN_MAX_NAME];
    strip_colon(name, assign_name, ASSIGN_MAX_NAME);
    if (!name[0]) return NULL;

    int ai = find_assign_idx(name);
    if (ai < 0) return NULL;
    if (idx >= g_assigns[ai].target_count) return NULL;
    return g_assigns[ai].targets[idx];
}

int VFS_ListAssigns(char *buf, int max)
{
    if (!buf || max < 2) return 0;

    int total = 0;
    buf[0] = '\0';

    for (int i = 0; i < MAX_ASSIGNS && total < max - 1; i++) {
        if (!g_assigns[i].valid) continue;

        if (total > 0) {
            if (total < max - 1) buf[total++] = '\n';
        }

        int ni = 0;
        while (total < max - 1 && g_assigns[i].name[ni]) {
            buf[total++] = g_assigns[i].name[ni++];
        }
        if (total < max - 1) buf[total++] = ':';
        if (total < max - 1) buf[total++] = ' ';
        if (total < max - 1) buf[total++] = '-';
        if (total < max - 1) buf[total++] = '>';
        if (total < max - 1) buf[total++] = ' ';

        for (int t = 0; t < g_assigns[i].target_count && total < max - 1; t++) {
            if (t > 0) {
                if (total < max - 1) buf[total++] = '\n';
                /* Indent and '+' for additional targets */
                if (total < max - 1) buf[total++] = ' ';
                if (total < max - 1) buf[total++] = ' ';
                if (total < max - 1) buf[total++] = '+';
                if (total < max - 1) buf[total++] = ' ';
            }
            int ti = 0;
            while (total < max - 1 && g_assigns[i].targets[t][ti]) {
                buf[total++] = g_assigns[i].targets[t][ti++];
            }
        }
    }
    buf[total] = '\0';
    return total;
}

/* Static buffer for expand result */
static char g_expand_buf[128];

const char *VFS_ExpandAssigns(const char *path, char *dst, int max)
{
    if (!path || !dst || max < 2) return NULL;

    /* Extract potential assign name from path */
    char vol_name[16];
    int vl = extract_vol(path, vol_name, 16);
    if (!vl) {
        /* No colon - not a volume/assign path, copy as-is */
        int i = 0;
        while (i < max - 1 && path[i]) { dst[i] = path[i]; i++; }
        dst[i] = '\0';
        return dst;
    }

    /* Check if it's an assign */
    const char *target = VFS_ResolveAssign(vol_name);
    if (target) {
        /* It's an assign - expand using first target */
        int ti = 0;
        while (ti < max - 1 && target[ti]) { dst[ti] = target[ti]; ti++; }
        /* Append rest of path after colon */
        const char *rest = path + vl + 1; /* Skip "VOL:" */
        if (*rest) {
            if (ti < max - 1) dst[ti++] = '/';
            while (ti < max - 1 && *rest) { dst[ti++] = *rest++; }
        }
        dst[ti] = '\0';
        return dst;
    }

    /* Not an assign - copy path as-is */
    int i = 0;
    while (i < max - 1 && path[i]) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    return dst;
}

/* -------------------------------------------------------------------------
 * AmigaDOS Handler Support
 * ------------------------------------------------------------------------- */

Handler *VFS_FindHandler(const char *vol_name)
{
    return find_handler(vol_name);
}

MsgPort *VFS_GetHandlerPort(const char *vol_name)
{
    Handler *h = find_handler(vol_name);
    return h ? &h->port : NULL;
}

RamFsVol *VFS_FindVol(const char *vol_name)
{
    return find_vol(vol_name);
}
