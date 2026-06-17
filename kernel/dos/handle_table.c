/* handle_table.c — Global open-file and lock handle table */

#include "handle_table.h"
#include "../exec/task.h"
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
    Forbid();
    uint32_t h = find_free_slot();
    if (h == 0) { Permit(); return 0; }

    HandleEntry *e = &g_entries[h - 1];
    e->type = HTYPE_FILE;
    scopy(e->path, path ? path : "", sizeof(e->path));
    e->u.file.fh    = *fh;
    e->u.file.flags = flags;
    Permit();
    return h;
}

uint32_t HandleTable_AllocLock(const char *path, void *node, int32_t access)
{
    Forbid();
    uint32_t h = find_free_slot();
    if (h == 0) { Permit(); return 0; }

    HandleEntry *e = &g_entries[h - 1];
    e->type = HTYPE_LOCK;
    scopy(e->path, path ? path : "", sizeof(e->path));
    e->u.lock.node      = node;
    e->u.lock.access    = access;
    e->u.lock.iter_next = NULL; /* caller must reset with LockResetIter */
    Permit();
    return h;
}

void HandleTable_Free(uint32_t handle)
{
    if (handle == 0 || handle > MAX_HANDLES) return;
    Forbid();
    g_entries[handle - 1].type = HTYPE_FREE;
    Permit();
}

HandleEntry *HandleTable_Get(uint32_t handle)
{
    if (handle == 0 || handle > MAX_HANDLES) return NULL;
    Forbid();
    HandleEntry *e = &g_entries[handle - 1];
    HandleEntry *result = (e->type != HTYPE_FREE) ? e : NULL;
    Permit();
    return result;
}

VfsFile *HandleTable_GetFile(uint32_t handle)
{
    HandleEntry *e = HandleTable_Get(handle);
    if (e && e->type == HTYPE_FILE)
        return &e->u.file.fh;
    return NULL;
}

HandleEntry *HandleTable_GetLockEntry(uint32_t handle, int32_t *access_out)
{
    HandleEntry *e = HandleTable_Get(handle);
    if (e && e->type == HTYPE_LOCK) {
        if (access_out) *access_out = e->u.lock.access;
        return e;
    }
    return NULL;
}

void *HandleTable_LockIterate(uint32_t handle)
{
    HandleEntry *e = HandleTable_Get(handle);
    if (!e || e->type != HTYPE_LOCK) return NULL;
    void *cur = e->u.lock.iter_next;
    /* iter_next advance is handler-specific; caller must update it */
    return cur;
}

void HandleTable_LockResetIter(uint32_t handle, void *first_child)
{
    HandleEntry *e = HandleTable_Get(handle);
    if (!e || e->type != HTYPE_LOCK) return;
    e->u.lock.iter_next = first_child;
}
