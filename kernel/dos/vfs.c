/* vfs.c — UAOS Virtual Filesystem Layer */

#include "vfs.h"
#include "ramfs.h"
#include "ram_handler.h"
#include "handle_table.h"
#include "boot/kprint.h"
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Mount table — maps "VOL" names to RamFsVol instances
 * ========================================================================= */

#define MAX_MOUNTS  16

typedef struct {
    char      vol_name[16]; /* e.g. "RAM" (no colon) */
    RamFsVol *vol;          /* direct pointer for native VFS access */
    Handler  *handler;      /* packet handler for DoPkt routing */
} MountEntry;

static MountEntry g_mounts[MAX_MOUNTS];
static int        g_n_mounts = 0;

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

    /* Direct volume lookup */
    for (int i = 0; i < g_n_mounts; i++)
        if (seq_ci(g_mounts[i].vol_name, name))
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
 * Writes resolved path to dst[max]. Returns dst or NULL on error. */
static const char *resolve_assign_path(const char *path, char *dst, int max)
{
    char vol_name[16];
    int vl = extract_vol(path, vol_name, 16);
    if (!vl) {
        /* No volume prefix - return as-is */
        int i = 0;
        while (i < max - 1 && path[i]) { dst[i] = path[i]; i++; }
        dst[i] = '\0';
        return dst;
    }

    const char *assign_target = VFS_ResolveAssign(vol_name);
    if (!assign_target) {
        /* Not an assign - return original path */
        int i = 0;
        while (i < max - 1 && path[i]) { dst[i] = path[i]; i++; }
        dst[i] = '\0';
        return dst;
    }

    /* Expand assign: "C:dir" -> "Workbench:C/dir" */
    int ti = 0;
    while (ti < max - 1 && assign_target[ti]) { dst[ti] = assign_target[ti]; ti++; }

    /* Append rest of path */
    const char *rest = path + vl + 1; /* skip "VOL:" */
    if (*rest) {
        if (ti < max - 1 && assign_target[ti-1] != ':') dst[ti++] = '/';
        while (ti < max - 1 && *rest) { dst[ti++] = *rest++; }
    }
    dst[ti] = '\0';
    return dst;
}

/* Forward declaration — defined in the Assign Support section below */
static const char *expand_with_target(const char *path, int vol_len,
                                      const char *target, char *dst, int max);

/* Register a mounted volume with an associated packet handler */
static void register_mount(const char *name, RamFsVol *vol, Handler *handler)
{
    if (g_n_mounts >= MAX_MOUNTS) return;
    int i = 0;
    while (i < 15 && name[i]) { g_mounts[g_n_mounts].vol_name[i] = name[i]; i++; }
    g_mounts[g_n_mounts].vol_name[i] = '\0';
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
    register_mount("RAM", ram, ram_handler);

    /* Standard AmigaDOS RAM disk directories */
    RamFS_MkDir(ram, "RAM:T");
    RamFS_MkDir(ram, "RAM:ENV");
    RamFS_MkDir(ram, "RAM:CLIPS");
    RamFS_MkDir(ram, "RAM:S");
}

/* Setup default Workbench assigns after Workbench: is mounted */
void VFS_SetupWorkbenchAssigns(void)
{
    /* Check if Workbench: is mounted */
    if (!find_vol("Workbench")) {
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
}

/* =========================================================================
 * Partition volume mounting (FAT32 partitions backed by empty RAMFS for now)
 * ========================================================================= */

int VFS_MountPartition(const char *name)
{
    if (!name || !*name) return -1;

    /* Check if already mounted */
    for (int i = 0; i < g_n_mounts; i++) {
        if (seq(g_mounts[i].vol_name, name))
            return 0;  /* already mounted */
    }

    if (g_n_mounts >= MAX_MOUNTS) return -1;

    RamFsVol *vol = RamFS_MountVol(name);
    if (!vol) return -1;

    Handler *handler = RamHandler_Create(name, vol);
    register_mount(name, vol, handler);
    return 0;
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
    register_mount(name, vol, handler);
    return 0;
}

int VFS_GetMountCount(void)
{
    return g_n_mounts;
}

int VFS_GetMountName(int idx, char *dst, int max)
{
    if (idx < 0 || idx >= g_n_mounts || !dst || max < 2) return 0;
    int i = 0;
    while (i < max - 1 && g_mounts[idx].vol_name[i]) {
        dst[i] = g_mounts[idx].vol_name[i];
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
    fh->node      = NULL;
    fh->pos       = 0;
    fh->nil       = 0;
    fh->handle_id = 0;

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

            char rvol[16];
            if (!extract_vol(resolved_path, rvol, 16)) continue;
            RamFsVol *vol = find_vol(rvol);
            if (!vol) continue;

            RamFsNode *node = RamFS_Resolve(vol, resolved_path);
            if (node && node->type == RAMFS_TYPE_FILE) {
                if (flags & VFS_TRUNC) node->size = 0;
                fh->node = node;
                fh->pos = 0;
                return 1;
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

    RamFsVol *vol = find_vol(rvol);
    if (!vol) return 0;

    RamFsNode *node = RamFS_Resolve(vol, resolved_path);

    if (!node) {
        if (!(flags & VFS_CREATE)) return 0;
        node = RamFS_Create(vol, resolved_path);
        if (!node) return 0;
    } else {
        if (node->type == RAMFS_TYPE_DIR) return 0; /* can't open dir as file */
        if (flags & VFS_TRUNC) node->size = 0;
    }

    fh->node = node;
    fh->pos  = 0;
    return 1;
}

void VFS_Close(VfsFile *fh)
{
    fh->node = NULL;
    fh->pos  = 0;
    fh->nil  = 0;
}

uint32_t VFS_Read(VfsFile *fh, uint8_t *buf, uint32_t len)
{
    if (fh->nil) return 0; /* EOF immediately */
    if (!fh->node) return 0;
    uint32_t got = RamFS_Read(fh->node, fh->pos, buf, len);
    fh->pos += got;
    return got;
}

/* Block size pre-allocated per file on first write.
 * 32 KB covers typical shell output and small tar archives. */
#define VFS_BLOCK_SZ  (32 * 1024)

uint32_t VFS_Write(VfsFile *fh, const uint8_t *buf, uint32_t len)
{
    if (fh->nil) return len; /* discard silently */
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
    if (!fh->node) return;
    fh->pos = (pos <= fh->node->size) ? pos : fh->node->size;
}

uint32_t VFS_Size(VfsFile *fh)
{
    if (fh->nil) return 0;
    if (!fh->node) return 0;
    return fh->node->size;
}

int VFS_MkDir(const char *path)
{
    char resolved_path[128];
    if (!resolve_assign_path(path, resolved_path, sizeof(resolved_path))) return -1;

    char vol_name[16];
    if (!extract_vol(resolved_path, vol_name, 16)) return -1;
    RamFsVol *vol = find_vol(vol_name);
    if (!vol) return -1;
    RamFsNode *node = RamFS_MkDir(vol, resolved_path);
    return node ? 0 : -1;
}

int VFS_Delete(const char *path)
{
    char resolved_path[128];
    if (!resolve_assign_path(path, resolved_path, sizeof(resolved_path))) return -1;

    char vol_name[16];
    if (!extract_vol(resolved_path, vol_name, 16)) return -1;
    RamFsVol *vol = find_vol(vol_name);
    if (!vol) return -1;
    return RamFS_Delete(vol, resolved_path);
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

RamFsNode *VFS_GetRoot(const char *vol_name)
{
    RamFsVol *vol = find_vol(vol_name);
    if (!vol) return NULL;
    return vol->root;
}

uint8_t VFS_GetAttrs(const char *path)
{
    char resolved_path[128];
    if (!resolve_assign_path(path, resolved_path, sizeof(resolved_path))) return 0;

    char vol_name[16];
    if (!extract_vol(resolved_path, vol_name, 16)) return 0;
    RamFsVol *vol = find_vol(vol_name);
    if (!vol) return 0;
    RamFsNode *node = RamFS_Resolve(vol, resolved_path);
    if (!node) return 0;
    return RamFS_GetAttrs(node);
}

int VFS_SetAttrs(const char *path, uint8_t attrs)
{
    char resolved_path[128];
    if (!resolve_assign_path(path, resolved_path, sizeof(resolved_path))) return -1;

    char vol_name[16];
    if (!extract_vol(resolved_path, vol_name, 16)) return -1;
    RamFsVol *vol = find_vol(vol_name);
    if (!vol) return -1;
    RamFsNode *node = RamFS_Resolve(vol, resolved_path);
    if (!node) return -1;
    return RamFS_SetAttrs(node, attrs);
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
    if (!vol) return -1;
    RamFsNode *node = RamFS_Resolve(vol, resolved_path);
    if (!node) return -1;
    int i = 0;
    while (i < max - 1 && node->comment[i]) { dst[i] = node->comment[i]; i++; }
    dst[i] = '\0';
    return 0;
}

int VFS_SetComment(const char *path, const char *comment)
{
    char resolved_path[128];
    if (!resolve_assign_path(path, resolved_path, sizeof(resolved_path))) return -1;
    char vol_name[16];
    if (!extract_vol(resolved_path, vol_name, 16)) return -1;
    RamFsVol *vol = find_vol(vol_name);
    if (!vol) return -1;
    RamFsNode *node = RamFS_Resolve(vol, resolved_path);
    if (!node) return -1;
    int i = 0;
    while (i < 63 && comment && comment[i]) { node->comment[i] = comment[i]; i++; }
    node->comment[i] = '\0';
    return 0;
}

int VFS_RenameVol(const char *old_name, const char *new_name)
{
    if (!old_name || !*old_name || !new_name || !*new_name) return -1;
    RamFsVol *vol = find_vol(old_name);
    if (!vol) return -1;
    return RamFS_RenameVol(vol, new_name);
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
    if (!vol) return -1;
    return RamFS_Rename(vol, old_res, new_res);
}

/* =========================================================================
 * AmigaDOS Assign Support
 * Assigns create logical names that map to physical paths.
 * Multi-assign (ADD) allows an assign to resolve to multiple directories.
 * Example: "Assign LIBS: Workbench:LIBS" then "Assign LIBS: SYS:Classes ADD"
 * makes "LIBS:foo" search Workbench:LIBS/foo then SYS:Classes/foo.
 * ========================================================================= */

#define MAX_ASSIGNS 16
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

    /* Validate target path exists unless DEFER is set */
    if (!defer) {
        char test_vol[16];
        if (!extract_vol(target_path, test_vol, 16)) return -1;
        if (!find_vol(test_vol)) return -1;
    }

    int idx = find_assign_idx(name);

    if (idx >= 0 && add) {
        /* Append to existing multi-assign */
        if (g_assigns[idx].target_count >= ASSIGN_MAX_TARGETS)
            return -1; /* Too many targets */
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
    if (free_idx < 0) return -1; /* Table full */

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
