/* handle_table.c — Global open-file and lock handle table */

#include "handle_table.h"
#include <stddef.h>

#define MAX_HANDLES 128

static HandleEntry g_entries[MAX_HANDLES];

void HandleTable_Init(void)
{
    for (int i = 0; i < MAX_HANDLES; i++)
        g_entries[i].type = HTYPE_FREE;
}

static void scopy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Handles are 1-based slot indices (0 = invalid).  Simple and fast. */
static uint32_t find_free_slot(void)
{
    for (int i = 0; i < MAX_HANDLES; i++)
        if (g_entries[i].type == HTYPE_FREE)
            return (uint32_t)(i + 1);
    return 0;
}

uint32_t HandleTable_AllocFile(const char *path, const VfsFile *fh, int flags)
{
    uint32_t h = find_free_slot();
    if (h == 0) return 0;

    HandleEntry *e = &g_entries[h - 1];
    e->type = HTYPE_FILE;
    scopy(e->path, path ? path : "", sizeof(e->path));
    e->u.file.fh    = *fh;
    e->u.file.flags = flags;
    return h;
}

uint32_t HandleTable_AllocLock(const char *path, RamFsNode *node, int32_t access)
{
    uint32_t h = find_free_slot();
    if (h == 0) return 0;

    HandleEntry *e = &g_entries[h - 1];
    e->type = HTYPE_LOCK;
    scopy(e->path, path ? path : "", sizeof(e->path));
    e->u.lock.node      = node;
    e->u.lock.access    = access;
    e->u.lock.iter_next = (node && node->type == RAMFS_TYPE_DIR)
                         ? node->first_child : NULL;
    return h;
}

void HandleTable_Free(uint32_t handle)
{
    if (handle == 0 || handle > MAX_HANDLES) return;
    g_entries[handle - 1].type = HTYPE_FREE;
}

HandleEntry *HandleTable_Get(uint32_t handle)
{
    if (handle == 0 || handle > MAX_HANDLES) return NULL;
    HandleEntry *e = &g_entries[handle - 1];
    return (e->type != HTYPE_FREE) ? e : NULL;
}

VfsFile *HandleTable_GetFile(uint32_t handle)
{
    HandleEntry *e = HandleTable_Get(handle);
    if (e && e->type == HTYPE_FILE)
        return &e->u.file.fh;
    return NULL;
}

RamFsNode *HandleTable_GetLock(uint32_t handle, int32_t *access_out)
{
    HandleEntry *e = HandleTable_Get(handle);
    if (e && e->type == HTYPE_LOCK) {
        if (access_out) *access_out = e->u.lock.access;
        return e->u.lock.node;
    }
    return NULL;
}

RamFsNode *HandleTable_LockIterate(uint32_t handle)
{
    HandleEntry *e = HandleTable_Get(handle);
    if (!e || e->type != HTYPE_LOCK) return NULL;
    RamFsNode *cur = e->u.lock.iter_next;
    if (cur) e->u.lock.iter_next = cur->next_sibling;
    return cur;
}

void HandleTable_LockResetIter(uint32_t handle)
{
    HandleEntry *e = HandleTable_Get(handle);
    if (!e || e->type != HTYPE_LOCK) return;
    RamFsNode *node = e->u.lock.node;
    e->u.lock.iter_next = (node && node->type == RAMFS_TYPE_DIR)
                         ? node->first_child : NULL;
}
