/* handle_table.c — Global open-file and lock handle table */

#include "handle_table.h"
#include "../exec/task.h"
#include "../dbg/diag.h"
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
    e->owner = Task_Current();
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
    e->owner = Task_Current();
    scopy(e->path, path ? path : "", sizeof(e->path));
    e->u.lock.node      = node;
    e->u.lock.access    = access;
    e->u.lock.iter_next = NULL; /* caller must reset with LockResetIter */
    Permit();
    return h;
}

uint32_t HandleTable_AllocMemFile(const char *path, uint32_t ram_addr,
                                  uint32_t size)
{
    Forbid();
    uint32_t h = find_free_slot();
    if (h == 0) { Permit(); return 0; }

    HandleEntry *e = &g_entries[h - 1];
    e->type = HTYPE_MEMFILE;
    e->owner = Task_Current();
    scopy(e->path, path ? path : "", sizeof(e->path));
    e->u.memfile.ram_addr = ram_addr;
    e->u.memfile.size     = size;
    e->u.memfile.pos      = 0;
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

/* Free every handle owned by `owner` (called from Task_Exit / RemTask —
 * UAOS-247).  A task that dies holding files or locks would otherwise leak
 * the table entries and the handler-side objects behind them.  Files get a
 * real VFS_Close (ACTION_END round-trip for handler-backed handles); locks
 * go through VFS_FreeLock so the handler frees its lock node too. */
uint32_t HandleTable_FreeByOwner(void *owner)
{
    if (!owner) return 0;
    uint32_t freed = 0;
    for (uint32_t i = 0; i < MAX_HANDLES; i++) {
        HandleEntry *e = &g_entries[i];
        if (e->type == HTYPE_FREE || e->owner != owner) continue;
        if (e->type == HTYPE_FILE) {
            VFS_Close(&e->u.file.fh);
            HandleTable_Free(i + 1);
        } else if (e->type == HTYPE_LOCK) {
            VFS_FreeLock(i + 1);
        } else {
            HandleTable_Free(i + 1);
        }
        freed++;
    }
    return freed;
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

/* -------------------------------------------------------------------------
 * C:handles — open-file/lock/handler-packet dump (UAOS-202)
 * ------------------------------------------------------------------------- */
void HandleTable_DiagDump(void *ctx, void (*emit)(void *, const char *))
{
    DiagLine l;
    int open = 0, leaked = 0;

    emit(ctx, " hnd  type  owner              pos/acc      flags  path");
    for (int i = 0; i < MAX_HANDLES; i++) {
        HandleEntry *e = &g_entries[i];
        if (e->type == HTYPE_FREE) continue;
        open++;

        /* A handle owned by a REMOVED task is a leak — the task can no
         * longer close it. */
        UaosTask *o = (UaosTask *)e->owner;
        int dead = o && o->tc_State == TASK_REMOVED;
        if (dead) leaked++;

        dl_reset(&l);
        dl_ch(&l, ' ');
        dl_dec(&l, (uint64_t)(i + 1)); dl_pad(&l, 5);
        dl_add(&l, e->type == HTYPE_FILE    ? "file"
                 : e->type == HTYPE_MEMFILE ? "memf" : "lock");
        dl_pad(&l, 12);
        dl_add(&l, (o && o->ln_Name) ? o->ln_Name : "-"); dl_pad(&l, 31);
        if (e->type == HTYPE_MEMFILE) {
            dl_dec(&l, e->u.memfile.pos);
            dl_pad(&l, 44);
            dl_add(&l, "ram");
        } else if (e->type == HTYPE_FILE) {
            dl_dec(&l, e->u.file.fh.pos);
            dl_pad(&l, 44);
            dl_dec(&l, (uint64_t)e->u.file.flags);
            if (e->u.file.fh.handler_port)
                dl_add(&l, " (pkt)");
            if (e->u.file.fh.nil) dl_add(&l, " (nil)");
        } else {
            dl_sdec(&l, e->u.lock.access);
            dl_pad(&l, 44);
            dl_add(&l, e->u.lock.access == EXCLUSIVE_LOCK ? "excl" : "shared");
        }
        dl_pad(&l, 51);
        dl_add(&l, e->path);
        if (dead) dl_add(&l, "  *LEAKED*");
        dl_emit(&l, ctx, emit);
    }

    dl_reset(&l);
    dl_add(&l, " ");
    dl_dec(&l, (uint64_t)open);
    dl_add(&l, " open, ");
    dl_dec(&l, (uint64_t)leaked);
    dl_add(&l, " owned by dead tasks");
    dl_emit(&l, ctx, emit);
}
