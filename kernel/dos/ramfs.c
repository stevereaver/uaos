/* ramfs.c — UAOS In-Memory RAM Filesystem */

#include "ramfs.h"
#include <stdint.h>
#include <stddef.h>

extern void kprint(const char *s);
extern void kprinthex(uint64_t v);
extern int g_virtio_irq_line;
extern unsigned int g_canary_before;
extern unsigned int g_canary_after;
#define CHECK_IRQ(label) do { int _irq = g_virtio_irq_line; unsigned int _cb = g_canary_before; unsigned int _ca = g_canary_after; if (_irq != 10) { kprint("[RAMFS] irq="); kprinthex(_irq); kprint(" at " label "\n"); } if (_cb != 0xDEADBEEF) { kprint("[RAMFS] CANARY_BEFORE="); kprinthex(_cb); kprint(" at " label "\n"); } if (_ca != 0xCAFEBABE) { kprint("[RAMFS] CANARY_AFTER="); kprinthex(_ca); kprint(" at " label "\n"); } } while(0)

/* =========================================================================
 * Static storage — all in BSS (zero-initialised)
 * ========================================================================= */

static RamFsNode  g_nodes[RAMFS_MAX_NODES];
static uint8_t    g_pool[1024 * 1024]; /* 1 MB shared data pool */
static uint32_t   g_pool_top = 0;                /* bump allocator cursor   */

#define MAX_VOLS  16
static RamFsVol   g_vols[MAX_VOLS];

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
            g_nodes[i].name[0]      = '\0';
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

/* Allocate bytes from the data pool */
uint8_t *RamFS_AllocPool(uint32_t bytes)
{
    uint32_t pool_sz = (uint32_t)sizeof(g_pool);
    if (g_pool_top + bytes > pool_sz) return NULL;
    uint8_t *p = &g_pool[g_pool_top];
    g_pool_top += bytes;
    return p;
}

static uint8_t *pool_alloc(uint32_t bytes) { return RamFS_AllocPool(bytes); }

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
    g_pool_top = 0;
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
    CHECK_IRQ("RamFS_MkDir start");
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
    scopy(node->name, last, RAMFS_MAX_NAME);
    node->parent = dir;
    dir_add_child(dir, node);
    CHECK_IRQ("RamFS_MkDir end");
    return node;
}

RamFsNode *RamFS_Create(RamFsVol *vol, const char *path)
{
    CHECK_IRQ("RamFS_Create start");
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
            return existing;
        }
        return NULL; /* exists as dir */
    }

    RamFsNode *node = alloc_node();
    if (!node) return NULL;
    node->type   = RAMFS_TYPE_FILE;
    scopy(node->name, last, RAMFS_MAX_NAME);
    node->parent = dir;
    node->size   = 0;
    node->alloc  = 0;
    node->data   = NULL;
    dir_add_child(dir, node);
    CHECK_IRQ("RamFS_Create end");
    return node;
}

int RamFS_Write(RamFsNode *node, const uint8_t *data, uint32_t len)
{
    if (!node || node->type != RAMFS_TYPE_FILE) return -1;
    if (node->attrs & RAMFS_ATTR_READONLY) return -2; /* read-only */
    if (node->ext_bdev) return -3; /* proxy file — read-only */
    if (len == 0) { node->size = 0; return 0; }

    if (len > node->alloc) {
        /* Try to allocate from pool */
        uint8_t *buf = pool_alloc(len);
        if (!buf) return -1;
        node->data  = buf;
        node->alloc = len;
    }

    for (uint32_t i = 0; i < len; i++) node->data[i] = data[i];
    node->size = len;
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
        /* Proxy file — read from block device on demand */
        uint32_t blksz = node->ext_blksz ? node->ext_blksz : 2048;
        uint32_t sector = node->ext_lba + (offset / blksz);
        uint32_t sec_off = offset % blksz;
        uint32_t total = 0;
        while (len > 0) {
            uint8_t sec_buf[4096];
            uint32_t ratio = blksz / node->ext_bdev->sector_size;
            uint64_t dev_sec = (uint64_t)sector * ratio;
            if (BlockDev_Read(node->ext_bdev, dev_sec, sec_buf, ratio) != 0)
                break;
            uint32_t chunk = blksz - sec_off;
            if (chunk > len) chunk = len;
            for (uint32_t i = 0; i < chunk; i++)
                buf[total + i] = sec_buf[sec_off + i];
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
    if (node->attrs & RAMFS_ATTR_READONLY) return -4; /* read-only */

    dir_remove_child(node->parent, node);
    node->type = RAMFS_TYPE_FREE;
    node->name[0] = '\0';
    node->attrs = 0;
    node->size  = 0;
    node->alloc = 0;
    node->data  = NULL;
    node->ext_bdev = NULL;
    node->ext_lba  = 0;
    node->ext_blksz = 0;
    node->parent = node->first_child = node->next_sibling = NULL;
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
