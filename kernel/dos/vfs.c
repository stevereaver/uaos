/* vfs.c — UAOS Virtual Filesystem Layer */

#include "vfs.h"
#include "ramfs.h"
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Mount table — maps "VOL" names to RamFsVol instances
 * ========================================================================= */

#define MAX_MOUNTS  4

typedef struct {
    char      vol_name[16]; /* e.g. "RAM" (no colon) */
    RamFsVol *vol;
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

/* Find mounted volume by name */
static RamFsVol *find_vol(const char *name)
{
    for (int i = 0; i < g_n_mounts; i++)
        if (seq(g_mounts[i].vol_name, name))
            return g_mounts[i].vol;
    return NULL;
}

/* Register a mounted volume */
static void register_mount(const char *name, RamFsVol *vol)
{
    if (g_n_mounts >= MAX_MOUNTS) return;
    int i = 0;
    while (i < 15 && name[i]) { g_mounts[g_n_mounts].vol_name[i] = name[i]; i++; }
    g_mounts[g_n_mounts].vol_name[i] = '\0';
    g_mounts[g_n_mounts].vol = vol;
    g_n_mounts++;
}

/* =========================================================================
 * Init — mount RAM: and create standard directories
 * ========================================================================= */

void VFS_Init(void)
{
    RamFS_Init();

    /* Mount RAM: */
    RamFsVol *ram = RamFS_MountVol("RAM");
    if (!ram) return;
    register_mount("RAM", ram);

    /* Standard AmigaDOS RAM disk directories */
    RamFS_MkDir(ram, "RAM:T");
    RamFS_MkDir(ram, "RAM:ENV");
    RamFS_MkDir(ram, "RAM:CLIPS");
    RamFS_MkDir(ram, "RAM:S");
}

/* =========================================================================
 * VFS_Open
 * ========================================================================= */

int VFS_Open(VfsFile *fh, const char *path, int flags)
{
    fh->node = NULL;
    fh->pos  = 0;

    char vol_name[16];
    if (!extract_vol(path, vol_name, 16)) return 0;

    RamFsVol *vol = find_vol(vol_name);
    if (!vol) return 0;

    RamFsNode *node = RamFS_Resolve(vol, path);

    if (!node) {
        if (!(flags & VFS_CREATE)) return 0;
        node = RamFS_Create(vol, path);
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
}

uint32_t VFS_Read(VfsFile *fh, uint8_t *buf, uint32_t len)
{
    if (!fh->node) return 0;
    uint32_t got = RamFS_Read(fh->node, fh->pos, buf, len);
    fh->pos += got;
    return got;
}

uint32_t VFS_Write(VfsFile *fh, const uint8_t *buf, uint32_t len)
{
    if (!fh->node || fh->node->type != RAMFS_TYPE_FILE) return 0;

    /* Simple append/overwrite from fh->pos */
    uint32_t end = fh->pos + len;
    if (end > RAMFS_MAX_FILESIZE) {
        len = RAMFS_MAX_FILESIZE - fh->pos;
        end = RAMFS_MAX_FILESIZE;
    }
    if (len == 0) return 0;

    /* Ensure the node has enough allocated space */
    if (end > fh->node->alloc) {
        /* Can't realloc in a bump allocator — write is only valid if
         * we're writing to a freshly created node from pos 0 */
        int rc = RamFS_Write(fh->node, buf, len);
        if (rc != 0) return 0;
        fh->pos += len;
        return len;
    }

    /* Write into existing allocation */
    for (uint32_t i = 0; i < len; i++)
        fh->node->data[fh->pos + i] = buf[i];
    fh->pos += len;
    if (fh->pos > fh->node->size)
        fh->node->size = fh->pos;
    return len;
}

void VFS_Seek(VfsFile *fh, uint32_t pos)
{
    if (!fh->node) return;
    fh->pos = (pos <= fh->node->size) ? pos : fh->node->size;
}

uint32_t VFS_Size(VfsFile *fh)
{
    if (!fh->node) return 0;
    return fh->node->size;
}

int VFS_MkDir(const char *path)
{
    char vol_name[16];
    if (!extract_vol(path, vol_name, 16)) return -1;
    RamFsVol *vol = find_vol(vol_name);
    if (!vol) return -1;
    RamFsNode *node = RamFS_MkDir(vol, path);
    return node ? 0 : -1;
}

int VFS_Delete(const char *path)
{
    char vol_name[16];
    if (!extract_vol(path, vol_name, 16)) return -1;
    RamFsVol *vol = find_vol(vol_name);
    if (!vol) return -1;
    return RamFS_Delete(vol, path);
}

RamFsNode *VFS_OpenDir(const char *path)
{
    char vol_name[16];
    if (!extract_vol(path, vol_name, 16)) return NULL;
    RamFsVol *vol = find_vol(vol_name);
    if (!vol) return NULL;
    RamFsNode *node = RamFS_Resolve(vol, path);
    if (!node || node->type != RAMFS_TYPE_DIR) return NULL;
    return node->first_child;
}

RamFsNode *VFS_ResolveDir(const char *path)
{
    char vol_name[16];
    int vl = extract_vol(path, vol_name, 16);

    /* Handle bare volume root like "RAM:" */
    if (vl > 0) {
        const char *after = path + vl + 1; /* skip colon */
        while (*after == '/') after++;
        if (*after == '\0') {
            /* Path is just the volume root */
            RamFsVol *vol = find_vol(vol_name);
            return vol ? vol->root : NULL;
        }
    }

    RamFsVol *vol = find_vol(vol_name);
    if (!vol) return NULL;
    RamFsNode *node = RamFS_Resolve(vol, path);
    if (!node || node->type != RAMFS_TYPE_DIR) return NULL;
    return node;
}

RamFsNode *VFS_GetRoot(const char *vol_name)
{
    RamFsVol *vol = find_vol(vol_name);
    if (!vol) return NULL;
    return vol->root;
}
