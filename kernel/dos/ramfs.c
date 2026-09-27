/* ramfs.c — UAOS In-Memory RAM Filesystem */

#include "ramfs.h"
#include "amiga_dos_types.h"
#include <stdint.h>
#include <stddef.h>

extern uint32_t ntp_get_epoch(void);

/* =========================================================================
 * Static storage — all in BSS (zero-initialised)
 * ========================================================================= */

static RamFsNode  g_nodes[RAMFS_MAX_NODES];
static uint8_t    g_pool[8 * 1024 * 1024] __attribute__((aligned(16)));
                                                    /* 8 MB shared data pool */
static uint32_t   g_pool_top = 0;   /* high-water mark: bytes above have never
                                     * been carved into a chunk              */

#define MAX_VOLS  16
static RamFsVol   g_vols[MAX_VOLS];

/* =========================================================================
 * Pool allocator — address-ordered free list with coalescing
 *
 * Every chunk (live or free) starts with a PoolChunk header recording its
 * total size.  Free chunks additionally carry 'next' (pool offset of the
 * next free chunk).  Allocation is first-fit on the free list, falling back
 * to carving a fresh chunk below g_pool_top.  Freeing inserts in address
 * order and merges with physically adjacent free neighbours.
 * ========================================================================= */

#define POOL_HDR    16u          /* sizeof(PoolChunk), keep in sync          */
#define POOL_ALIGN  16u
#define POOL_NIL    0xFFFFFFFFu  /* free-list terminator (offset form)       */

typedef struct {
    uint32_t size;               /* chunk size in bytes, header included    */
    uint32_t next;               /* free chunks: offset of next free chunk  */
    uint64_t _pad;
} PoolChunk;

static uint32_t g_free_head = POOL_NIL;  /* offset of first free chunk      */
static uint32_t g_pool_used = 0;         /* bytes held by live allocations  */

/* =========================================================================
 * Helpers
 * ========================================================================= */

static int slen(const char *s)
{
    int n = 0; while (s[n]) n++; return n;
}

static int seq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int seq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca += 32;
    if (cb >= 'A' && cb <= 'Z') cb += 32;
    return ca == cb;
}

static void scopy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Copy the next path component from *p into comp (up to max-1 chars).
 * Advances *p past the component and any trailing '/'.
 * Returns 0 if nothing left. */
static int next_component(const char **p, char *comp, int max)
{
    const char *s = *p;
    if (!*s) return 0;
    int i = 0;
    while (*s && *s != '/' && i < max - 1)
        comp[i++] = *s++;
    comp[i] = '\0';
    if (*s == '/') s++;
    *p = s;
    return i > 0;
}

/* Allocate a free node from the pool.  Returns NULL if full. */
static RamFsNode *alloc_node(void)
{
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (g_nodes[i].type == RAMFS_TYPE_FREE) {
            g_nodes[i].type         = 0xFF; /* mark claimed, caller sets type */
            g_nodes[i].attrs        = 0;   /* no attributes */
            g_nodes[i].protection   = DEFAULT_PROTECTION;
            g_nodes[i].name[0]      = '\0';
            g_nodes[i].comment[0]   = '\0';
            g_nodes[i].mtime        = 0;
            g_nodes[i].parent       = NULL;
            g_nodes[i].first_child  = NULL;
            g_nodes[i].next_sibling = NULL;
            g_nodes[i].data         = NULL;
            g_nodes[i].size         = 0;
            g_nodes[i].alloc        = 0;
            g_nodes[i].ext_bdev     = NULL;
            g_nodes[i].ext_lba      = 0;
            g_nodes[i].ext_blksz    = 0;
            return &g_nodes[i];
        }
    }
    return NULL;
}

/* Append child to dir's child list */
static void dir_add_child(RamFsNode *dir, RamFsNode *child)
{
    child->next_sibling = NULL;
    if (!dir->first_child) {
        dir->first_child = child;
        return;
    }
    RamFsNode *cur = dir->first_child;
    while (cur->next_sibling) cur = cur->next_sibling;
    cur->next_sibling = child;
}

/* Remove child from dir's child list */
static void dir_remove_child(RamFsNode *dir, RamFsNode *child)
{
    if (dir->first_child == child) {
        dir->first_child = child->next_sibling;
        return;
    }
    RamFsNode *cur = dir->first_child;
    while (cur && cur->next_sibling != child) cur = cur->next_sibling;
    if (cur) cur->next_sibling = child->next_sibling;
}

/* Find a direct child of dir by name */
static RamFsNode *find_child(RamFsNode *dir, const char *name)
{
    RamFsNode *c = dir->first_child;
    while (c) {
        if (seq_ci(c->name, name)) return c;
        c = c->next_sibling;
    }
    return NULL;
}

/* Allocate bytes from the data pool.  Returns NULL when no chunk fits. */
uint8_t *RamFS_AllocPool(uint32_t bytes)
{
    if (!bytes) return NULL;
    uint32_t need = POOL_HDR + ((bytes + POOL_ALIGN - 1) & ~(POOL_ALIGN - 1));

    /* First-fit over the (address-ordered) free list */
    uint32_t prev = POOL_NIL, cur = g_free_head;
    while (cur != POOL_NIL) {
        PoolChunk *c = (PoolChunk *)&g_pool[cur];
        if (c->size >= need) {
            uint32_t rem = c->size - need;
            if (rem >= POOL_HDR + POOL_ALIGN) {
                /* Split: remainder becomes a free chunk in c's slot */
                PoolChunk *r = (PoolChunk *)&g_pool[cur + need];
                r->size = rem;
                r->next = c->next;
                c->size = need;
                if (prev == POOL_NIL) g_free_head = cur + need;
                else ((PoolChunk *)&g_pool[prev])->next = cur + need;
            } else {
                /* Consume the whole chunk */
                if (prev == POOL_NIL) g_free_head = c->next;
                else ((PoolChunk *)&g_pool[prev])->next = c->next;
            }
            g_pool_used += c->size;
            return (uint8_t *)(c + 1);
        }
        prev = cur;
        cur = c->next;
    }

    /* No reusable chunk — carve fresh space below the high-water mark */
    uint32_t pool_sz = (uint32_t)sizeof(g_pool);
    if (g_pool_top + need > pool_sz) return NULL;
    PoolChunk *c = (PoolChunk *)&g_pool[g_pool_top];
    g_pool_top += need;
    c->size = need;
    g_pool_used += need;
    return (uint8_t *)(c + 1);
}

/* Return a pool allocation to the free list (NULL-safe). */
void RamFS_FreePool(uint8_t *ptr)
{
    if (!ptr) return;
    PoolChunk *c = ((PoolChunk *)ptr) - 1;
    uint32_t off = (uint32_t)((uint8_t *)c - g_pool);
    g_pool_used -= c->size;

    /* Find insertion point (address-ordered list) */
    uint32_t prev = POOL_NIL, cur = g_free_head;
    while (cur != POOL_NIL && cur < off) {
        prev = cur;
        cur = ((PoolChunk *)&g_pool[cur])->next;
    }

    /* Merge with the following chunk when physically adjacent */
    if (cur != POOL_NIL && off + c->size == cur) {
        c->size += ((PoolChunk *)&g_pool[cur])->size;
        c->next  = ((PoolChunk *)&g_pool[cur])->next;
    } else {
        c->next = cur;
    }

    /* Link in, merging with the preceding chunk when adjacent */
    if (prev != POOL_NIL) {
        PoolChunk *p = (PoolChunk *)&g_pool[prev];
        if (prev + p->size == off) {
            p->size += c->size;
            p->next  = c->next;
        } else {
            p->next = off;
        }
    } else {
        g_free_head = off;
    }
}

static uint8_t *pool_alloc(uint32_t bytes) { return RamFS_AllocPool(bytes); }
static void     pool_free(uint8_t *ptr)    { RamFS_FreePool(ptr); }

/* Last-block cache for ext_bdev proxy reads (see RamFS_Read).  Keyed on
 * (bdev, fs block size, device-sector start); ext-backed files are
 * read-only mounts so no write invalidation is needed. */
static BlockDev *g_sec_bdev   = NULL;
static uint64_t  g_sec_devsec = 0;
static uint32_t  g_sec_blksz  = 0;
static int       g_sec_valid  = 0;
static uint8_t   g_sec_buf[8192];

/* =========================================================================
 * Path resolution
 * Strips the "VOL:" prefix then walks the node tree.
 * ========================================================================= */

/* Skip "VOL:" prefix, return pointer to the path after the colon.
 * If there's no colon, returns the full string (relative). */
static const char *skip_vol_prefix(const char *path)
{
    const char *p = path;
    while (*p && *p != ':') p++;
    if (*p == ':') return p + 1;
    return path;
}

static RamFsNode *resolve_from(RamFsNode *dir, const char *path)
{
    /* Empty path = dir itself */
    if (!path || !*path) return dir;

    char comp[RAMFS_MAX_NAME];
    const char *p = path;

    while (next_component(&p, comp, RAMFS_MAX_NAME)) {
        RamFsNode *child = find_child(dir, comp);
        if (!child) return NULL;
        dir = child;
    }
    return dir;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void RamFS_Init(void)
{
    /* BSS is zero — nodes are RAMFS_TYPE_FREE (0) by default */
    g_pool_top  = 0;
    g_free_head = POOL_NIL;
    g_pool_used = 0;
    g_sec_valid = 0;
}

RamFsVol *RamFS_MountVol(const char *name)
{
    for (int i = 0; i < MAX_VOLS; i++) {
        if (!g_vols[i].valid) {
            scopy(g_vols[i].name, name, 16);
            RamFsNode *root = alloc_node();
            if (!root) return NULL;
            root->type   = RAMFS_TYPE_DIR;
            root->name[0] = '\0'; /* root has no name */
            g_vols[i].root  = root;
            g_vols[i].valid = 1;
            return &g_vols[i];
        }
    }
    return NULL;
}

RamFsNode *RamFS_Resolve(RamFsVol *vol, const char *path)
{
    if (!vol || !vol->valid) return NULL;
    const char *p = skip_vol_prefix(path);
    /* Leading slash optional */
    if (*p == '/') p++;
    return resolve_from(vol->root, p);
}

RamFsNode *RamFS_MkDir(RamFsVol *vol, const char *path)
{
    if (!vol || !vol->valid) return NULL;
    const char *p = skip_vol_prefix(path);
    if (*p == '/') p++;

    /* Walk to parent, create the final component */
    char comp[RAMFS_MAX_NAME];
    RamFsNode *dir = vol->root;
    const char *cur = p;

    /* Consume all but the last component, creating dirs as needed */
    char last[RAMFS_MAX_NAME];
    last[0] = '\0';
    const char *prev = cur;
    while (next_component(&cur, comp, RAMFS_MAX_NAME)) {
        scopy(last, comp, RAMFS_MAX_NAME);
        if (*cur) { /* more components remain — navigate/create intermediate */
            RamFsNode *child = find_child(dir, comp);
            if (!child) {
                child = alloc_node();
                if (!child) return NULL;
                child->type   = RAMFS_TYPE_DIR;
                child->mtime  = ntp_get_epoch();
                scopy(child->name, comp, RAMFS_MAX_NAME);
                child->parent = dir;
                dir_add_child(dir, child);
            }
            dir = child;
        }
        prev = cur;
    }
    (void)prev;

    if (!last[0]) return NULL; /* no name given */

    /* Check not already exists */
    RamFsNode *existing = find_child(dir, last);
    if (existing) return existing; /* idempotent */

    RamFsNode *node = alloc_node();
    if (!node) return NULL;
    node->type   = RAMFS_TYPE_DIR;
    node->mtime  = ntp_get_epoch();
    scopy(node->name, last, RAMFS_MAX_NAME);
    node->parent = dir;
    dir_add_child(dir, node);
    return node;
}

RamFsNode *RamFS_Create(RamFsVol *vol, const char *path)
{
    if (!vol || !vol->valid) return NULL;
    const char *p = skip_vol_prefix(path);
    if (*p == '/') p++;

    /* Find parent directory and leaf name */
    char comp[RAMFS_MAX_NAME];
    char last[RAMFS_MAX_NAME];
    last[0] = '\0';
    RamFsNode *dir = vol->root;
    const char *cur = p;

    while (next_component(&cur, comp, RAMFS_MAX_NAME)) {
        scopy(last, comp, RAMFS_MAX_NAME);
        if (*cur) {
            RamFsNode *child = find_child(dir, comp);
            if (!child || child->type != RAMFS_TYPE_DIR) return NULL;
            dir = child;
        }
    }

    if (!last[0]) return NULL;

    /* If already exists as a file, reuse (truncate) */
    RamFsNode *existing = find_child(dir, last);
    if (existing) {
        if (existing->type == RAMFS_TYPE_FILE) {
            existing->size = 0;
            existing->mtime = ntp_get_epoch();
            return existing;
        }
        return NULL; /* exists as dir */
    }

    RamFsNode *node = alloc_node();
    if (!node) return NULL;
    node->type   = RAMFS_TYPE_FILE;
    node->mtime  = ntp_get_epoch();
    scopy(node->name, last, RAMFS_MAX_NAME);
    node->parent = dir;
    node->size   = 0;
    node->alloc  = 0;
    node->data   = NULL;
    dir_add_child(dir, node);
    return node;
}

int RamFS_Write(RamFsNode *node, const uint8_t *data, uint32_t len)
{
    if (!node || node->type != RAMFS_TYPE_FILE) return -1;
    if (node->attrs & RAMFS_ATTR_READONLY) return -2; /* read-only */
    if (node->ext_bdev) return -3; /* proxy file — read-only */
    if (len == 0) { node->size = 0; return 0; }

    if (len > node->alloc) {
        /* Grow: allocate a fresh buffer, release the old one */
        uint8_t *buf = pool_alloc(len);
        if (!buf) return -1;
        pool_free(node->data);
        node->data  = buf;
        node->alloc = len;
    }

    for (uint32_t i = 0; i < len; i++) node->data[i] = data[i];
    node->size = len;
    node->mtime = ntp_get_epoch();
    return 0;
}

uint32_t RamFS_Read(RamFsNode *node, uint32_t offset,
                    uint8_t *buf, uint32_t len)
{
    if (!node || node->type != RAMFS_TYPE_FILE) return 0;
    if (offset >= node->size) return 0;
    uint32_t avail = node->size - offset;
    if (len > avail) len = avail;

    if (node->ext_bdev) {
        /* Proxy file — read from block device on demand.  The last
         * filesystem block read is cached: sequential readers issue many
         * small reads that all land in the same block, and this avoids
         * re-fetching it from the device every call. */
        uint32_t blksz = node->ext_blksz ? node->ext_blksz : 2048;
        if (blksz > sizeof(g_sec_buf)) return 0;
        uint32_t sector = node->ext_lba + (offset / blksz);
        uint32_t sec_off = offset % blksz;
        uint32_t ratio = blksz / node->ext_bdev->sector_size;
        uint32_t total = 0;
        while (len > 0) {
            uint64_t dev_sec = (uint64_t)sector * ratio;
            if (!(g_sec_valid && g_sec_bdev == node->ext_bdev &&
                  g_sec_blksz == blksz && g_sec_devsec == dev_sec)) {
                if (BlockDev_Read(node->ext_bdev, dev_sec, g_sec_buf,
                                  ratio) != 0)
                    break;
                g_sec_bdev   = node->ext_bdev;
                g_sec_blksz  = blksz;
                g_sec_devsec = dev_sec;
                g_sec_valid  = 1;
            }
            uint32_t chunk = blksz - sec_off;
            if (chunk > len) chunk = len;
            for (uint32_t i = 0; i < chunk; i++)
                buf[total + i] = g_sec_buf[sec_off + i];
            total += chunk;
            len -= chunk;
            sec_off = 0;
            sector++;
        }
        return total;
    }

    for (uint32_t i = 0; i < len; i++) buf[i] = node->data[offset + i];
    return len;
}

int RamFS_Delete(RamFsVol *vol, const char *path)
{
    RamFsNode *node = RamFS_Resolve(vol, path);
    if (!node) return -1;
    if (node->type == RAMFS_TYPE_DIR && node->first_child) return -2; /* not empty */
    if (!node->parent) return -3; /* cannot delete root */
    if (node->protection & FIBF_DELETE) return -4; /* delete-protected */

    pool_free(node->data); /* return file data to the pool */
    dir_remove_child(node->parent, node);
    node->type = RAMFS_TYPE_FREE;
    node->name[0] = '\0';
    node->attrs = 0;
    node->protection = DEFAULT_PROTECTION;
    node->size  = 0;
    node->alloc = 0;
    node->data  = NULL;
    node->ext_bdev = NULL;
    node->ext_lba  = 0;
    node->ext_blksz = 0;
    node->parent = node->first_child = node->next_sibling = NULL;
    return 0;
}

/* Rename / move a node within the same volume.
 * Returns 0 on success, negative on error. */
int RamFS_Rename(RamFsVol *vol, const char *old_path, const char *new_path)
{
    if (!vol || !vol->valid) return -1;

    /* Resolve source */
    RamFsNode *src = RamFS_Resolve(vol, old_path);
    if (!src) return -1;
    if (!src->parent) return -3; /* cannot rename root */
    if (src->protection & FIBF_DELETE) return -4;

    /* Parse destination into parent directory + leaf name */
    const char *p = skip_vol_prefix(new_path);
    if (*p == '/') p++;

    char comp[RAMFS_MAX_NAME];
    char last[RAMFS_MAX_NAME];
    last[0] = '\0';
    RamFsNode *dst_dir = vol->root;
    const char *cur = p;

    while (next_component(&cur, comp, RAMFS_MAX_NAME)) {
        scopy(last, comp, RAMFS_MAX_NAME);
        if (*cur) {
            RamFsNode *child = find_child(dst_dir, comp);
            if (!child || child->type != RAMFS_TYPE_DIR) return -1; /* path invalid */
            dst_dir = child;
        }
    }
    if (!last[0]) return -1; /* no destination name */

    /* Check if destination already exists */
    RamFsNode *existing = find_child(dst_dir, last);
    if (existing) {
        if (existing == src) return 0; /* no-op */
        /* AmigaDOS: overwrite only if same type and not a non-empty dir */
        if (existing->type == RAMFS_TYPE_DIR && existing->first_child)
            return -2; /* destination dir not empty */
        if (existing->protection & FIBF_DELETE) return -4;
        /* Remove existing node (returning any file data to the pool) */
        pool_free(existing->data);
        dir_remove_child(existing->parent, existing);
        existing->type = RAMFS_TYPE_FREE;
        existing->parent = existing->first_child = existing->next_sibling = NULL;
    }

    /* If moving to a different parent, unlink from old and link to new */
    if (src->parent != dst_dir) {
        dir_remove_child(src->parent, src);
        src->parent = dst_dir;
        dir_add_child(dst_dir, src);
    }

    /* Update name */
    scopy(src->name, last, RAMFS_MAX_NAME);
    return 0;
}

RamFsNode *RamFS_FirstChild(RamFsNode *dir)
{
    if (!dir || dir->type != RAMFS_TYPE_DIR) return NULL;
    return dir->first_child;
}

uint8_t RamFS_GetAttrs(RamFsNode *node)
{
    if (!node) return 0;
    return node->attrs;
}

int RamFS_SetAttrs(RamFsNode *node, uint8_t attrs)
{
    if (!node) return -1;
    node->attrs = attrs;
    return 0;
}

uint16_t RamFS_GetProtection(RamFsNode *node)
{
    if (!node) return DEFAULT_PROTECTION;
    return node->protection;
}

int RamFS_SetProtection(RamFsNode *node, uint16_t prot)
{
    if (!node) return -1;
    node->protection = prot;
    return 0;
}

int RamFS_RenameVol(RamFsVol *vol, const char *new_name)
{
    if (!vol || !vol->valid || !new_name || !*new_name) return -1;
    int i = 0;
    while (i < 15 && new_name[i]) { vol->name[i] = new_name[i]; i++; }
    vol->name[i] = '\0';
    return 0;
}

void RamFS_GetVolumeStats(RamFsVol *vol, uint32_t *total_bytes, uint32_t *used_bytes)
{
    /* Per-volume used: walk this volume's node tree, summing pool bytes
     * held by files that actually consume pool.  ext_bdev proxy files are
     * skipped — their content lives on the backing block device. */
    uint32_t used = 0;
    if (vol && vol->root) {
        RamFsNode *n = vol->root;
        while (n) {
            if (n->type == RAMFS_TYPE_FILE && !n->ext_bdev)
                used += n->alloc;
            /* Iterative DFS: child, else sibling, else climb to a sibling */
            RamFsNode *next = n->first_child ? n->first_child
                                             : n->next_sibling;
            while (!next && n->parent) {
                n = n->parent;
                next = n->next_sibling;
            }
            n = next;
        }
    }

    /* All RAM volumes draw from the same pool: total is this volume's own
     * usage plus whatever the pool can still supply (free = pool_sz -
     * pool_used, including header overhead and unreclaimed fragments). */
    uint32_t pool_sz  = (uint32_t)sizeof(g_pool);
    uint32_t pool_free = pool_sz - g_pool_used;
    *used_bytes  = used;
    *total_bytes = used + pool_free;
}
