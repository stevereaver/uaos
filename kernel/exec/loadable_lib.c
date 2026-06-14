/*
 * loadable_lib.c — UAOS Loadable Library System
 *
 * Scans Workbench:LIBS/ for *.library descriptor files at boot,
 * parses the UAOS binary format, and maintains a registry of
 * loadable libraries that the m68k glue layer can dispatch to.
 */

#include "loadable_lib.h"
#include "dos/vfs.h"
#include "boot/kprint.h"
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Registry
 * ------------------------------------------------------------------------- */

typedef struct {
    char        name[UAOS_MAX_LIB_NAME];
    uint16_t    version;
    uint16_t    func_count;
    uint32_t    base_addr;
    uint8_t     lib_id;
    uint8_t     active;
    void      (*dispatch)(uint32_t fn_idx);
} LoadableLibEntry;

static LoadableLibEntry g_libs[UAOS_MAX_LOADABLE_LIBS];
static int              g_lib_count = 0;
static uint8_t          g_next_lib_id = 6;  /* 1-5 reserved for built-in libs */
static uint32_t           g_next_base = 0x6000;

/* -------------------------------------------------------------------------
 * Tiny freestanding helpers
 * ------------------------------------------------------------------------- */

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int seq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int ends_with(const char *s, const char *suffix)
{
    int sl = slen(s);
    int sufl = slen(suffix);
    if (sl < sufl) return 0;
    for (int i = 0; i < sufl; i++)
        if (s[sl - sufl + i] != suffix[i]) return 0;
    return 1;
}

static void scopy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void scat(char *dst, const char *src, int max)
{
    int d = 0;
    while (d < max - 1 && dst[d]) d++;
    int i = 0;
    while (d < max - 1 && src[i]) { dst[d++] = src[i++]; }
    dst[d] = '\0';
}

static uint16_t read_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

/* -------------------------------------------------------------------------
 * Parse a single .library file
 * ------------------------------------------------------------------------- */

static int parse_library_file(const char *path)
{
    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ)) {
        kprint("[LOADABLE] Failed to open ");
        kprint(path);
        kprint("\n");
        return -1;
    }

    uint8_t buf[256];
    uint32_t got = VFS_Read(&fh, buf, sizeof(buf));
    VFS_Close(&fh);

    if (got < 12) {
        kprint("[LOADABLE] File too small: ");
        kprint(path);
        kprint("\n");
        return -1;
    }

    if (buf[0] != UAOS_LIB_MAGIC_0 || buf[1] != UAOS_LIB_MAGIC_1 ||
        buf[2] != UAOS_LIB_MAGIC_2 || buf[3] != UAOS_LIB_MAGIC_3) {
        kprint("[LOADABLE] Bad magic in ");
        kprint(path);
        kprint("\n");
        return -1;
    }

    uint16_t file_ver = read_be16(buf + 4);
    if (file_ver != 1) {
        kprint("[LOADABLE] Unknown version in ");
        kprint(path);
        kprint("\n");
        return -1;
    }

    uint16_t name_len = read_be16(buf + 6);
    if (name_len < 1 || name_len > UAOS_MAX_LIB_NAME - 1 ||
        (uint32_t)(8 + name_len + 4) > got) {
        kprint("[LOADABLE] Bad name length in ");
        kprint(path);
        kprint("\n");
        return -1;
    }

    char name[UAOS_MAX_LIB_NAME];
    int nl = 0;
    while (nl < name_len - 1 && nl < UAOS_MAX_LIB_NAME - 1 &&
           buf[8 + nl] != '\0') {
        name[nl] = (char)buf[8 + nl];
        nl++;
    }
    name[nl] = '\0';

    int name_off = 8 + ((name_len + 1) & ~1u);
    uint16_t lib_ver      = read_be16(buf + name_off);
    uint16_t func_count   = read_be16(buf + name_off + 2);

    if (g_lib_count >= UAOS_MAX_LOADABLE_LIBS) {
        kprint("[LOADABLE] Registry full, cannot load ");
        kprint(name);
        kprint("\n");
        return -1;
    }

    LoadableLibEntry *e = &g_libs[g_lib_count++];
    scopy(e->name, name, UAOS_MAX_LIB_NAME);
    e->version    = lib_ver;
    e->func_count = func_count;
    e->base_addr  = g_next_base;
    e->lib_id     = g_next_lib_id++;
    e->active     = 1;
    e->dispatch   = NULL;

    g_next_base += 0x1000;

    kprint("[LOADABLE] Registered ");
    kprint(name);
    kprint(" v");
    kprintdec(lib_ver);
    kprint(" (");
    kprintdec(func_count);
    kprint(" funcs) base=0x");
    kprinthex(e->base_addr);
    kprint("\n");

    return 0;
}

/* -------------------------------------------------------------------------
 * Boot scan
 * ------------------------------------------------------------------------- */

void UAOS_LoadableLib_Init(void)
{
    kprint("[LOADABLE] Scanning LIBS: for .library files...\n");

    for (int i = 0; i < UAOS_MAX_LOADABLE_LIBS; i++)
        g_libs[i].active = 0;
    g_lib_count = 0;
    g_next_lib_id = 6;
    g_next_base = 0x6000;

    RamFsNode *node = VFS_OpenDir("Workbench:LIBS");
    if (!node) {
        kprint("[LOADABLE] Workbench:LIBS not found.\n");
        return;
    }

    int found = 0;
    while (node) {
        if (node->type == RAMFS_TYPE_FILE && ends_with(node->name, ".library")) {
            char path[128];
            scopy(path, "Workbench:LIBS/", 128);
            scat(path, node->name, 128);
            parse_library_file(path);
            found++;
        }
        node = node->next_sibling;
    }

    if (found == 0)
        kprint("[LOADABLE] No .library files found.\n");
}

/* -------------------------------------------------------------------------
 * Look-up
 * ------------------------------------------------------------------------- */

LoadableLibInfo *UAOS_LoadableLib_FindByName(const char *name)
{
    for (int i = 0; i < g_lib_count; i++) {
        if (g_libs[i].active && seq(g_libs[i].name, name))
            return (LoadableLibInfo *)&g_libs[i]; /* same layout */
    }
    return NULL;
}

LoadableLibInfo *UAOS_LoadableLib_GetById(uint8_t lib_id)
{
    for (int i = 0; i < g_lib_count; i++) {
        if (g_libs[i].active && g_libs[i].lib_id == lib_id)
            return (LoadableLibInfo *)&g_libs[i];
    }
    return NULL;
}

int UAOS_LoadableLib_GetInfo(int idx, LoadableLibInfo *out)
{
    if (!out || idx < 0 || idx >= g_lib_count) return 0;
    if (!g_libs[idx].active) return 0;
    out->name       = g_libs[idx].name;
    out->version    = g_libs[idx].version;
    out->func_count = g_libs[idx].func_count;
    out->base_addr  = g_libs[idx].base_addr;
    out->lib_id     = g_libs[idx].lib_id;
    out->dispatch   = g_libs[idx].dispatch;
    return 1;
}

/* -------------------------------------------------------------------------
 * Bind dispatch
 * ------------------------------------------------------------------------- */

int UAOS_LoadableLib_BindDispatch(const char *name,
                                  void (*dispatch)(uint32_t fn_idx))
{
    for (int i = 0; i < g_lib_count; i++) {
        if (g_libs[i].active && seq(g_libs[i].name, name)) {
            g_libs[i].dispatch = dispatch;
            kprint("[LOADABLE] Bound native dispatch for ");
            kprint(name);
            kprint("\n");
            return 0;
        }
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Listing
 * ------------------------------------------------------------------------- */

int UAOS_LoadableLib_ListAll(char *names[], uint16_t versions[], int max_count)
{
    if (!names || !versions || max_count <= 0) return g_lib_count;
    int copy = (g_lib_count < max_count) ? g_lib_count : max_count;
    for (int i = 0; i < copy; i++) {
        names[i]     = g_libs[i].name;
        versions[i]  = g_libs[i].version;
    }
    return g_lib_count;
}

int UAOS_LoadableLib_Count(void)
{
    return g_lib_count;
}
