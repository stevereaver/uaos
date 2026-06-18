/* handler_loader.c — Handler loader implementation
 *
 * Maintains a table of LHandlerEntry records.  Scans the L: assign
 * (resolved to Workbench:L/) for handler binaries and supports both
 * loaded M68k binaries and built-in native handlers.
 */

#include "handler_loader.h"
#include "vfs.h"
#include "ramfs.h"
#include "dospacket.h"
#include "boot/kprint.h"
#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Static table
 * ------------------------------------------------------------------------- */

static LHandlerEntry g_lhandlers[MAX_L_HANDLERS];
static int           g_lhandler_count = 0;

/* -------------------------------------------------------------------------
 * String helpers (no libc)
 * ------------------------------------------------------------------------- */

static int hlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void hcpy(char *d, const char *s, int max)
{
    int i = 0;
    while (i < max - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static int hcmp(const char *a, const char *b)
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

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

static LHandlerEntry *alloc_entry(void)
{
    if (g_lhandler_count >= MAX_L_HANDLERS) return NULL;
    LHandlerEntry *e = &g_lhandlers[g_lhandler_count++];
    for (int i = 0; i < (int)sizeof(LHandlerEntry); i++)
        ((uint8_t *)e)[i] = 0;
    return e;
}

static LHandlerEntry *find_by_name(const char *name)
{
    for (int i = 0; i < g_lhandler_count; i++)
        if (hcmp(g_lhandlers[i].name, name))
            return &g_lhandlers[i];
    return NULL;
}

static LHandlerEntry *find_by_device(const char *device_name)
{
    for (int i = 0; i < g_lhandler_count; i++)
        if (hcmp(g_lhandlers[i].device_name, device_name))
            return &g_lhandlers[i];
    return NULL;
}

/* -------------------------------------------------------------------------
 * Built-in native handler forward declarations
 * ------------------------------------------------------------------------- */
extern Handler *AuxHandler_Create(const char *name);
extern Handler *PortHandler_Create(const char *name);

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void HandlerLoader_Init(void)
{
    g_lhandler_count = 0;
    for (int i = 0; i < MAX_L_HANDLERS; i++) {
        LHandlerEntry *e = &g_lhandlers[i];
        for (int j = 0; j < (int)sizeof(LHandlerEntry); j++)
            ((uint8_t *)e)[j] = 0;
    }
}

int HandlerLoader_ScanLDirectory(void)
{
    /* Resolve L: to its physical path */
    char l_path[64];
    const char *resolved = VFS_ExpandAssigns("L:", l_path, sizeof(l_path));
    if (!resolved) {
        kprint("[HandlerLoader] L: not assigned, skipping scan\n");
        return 0;
    }

    RamFsNode *child = VFS_OpenDir(resolved);
    if (!child) {
        kprint("[HandlerLoader] L: directory not found: ");
        kprint(resolved);
        kprint("\n");
        return 0;
    }

    int found = 0;
    while (child) {
        /* Only consider regular files (not directories) as handler binaries */
        if (child->type == RAMFS_TYPE_FILE) {
            LHandlerEntry *e = alloc_entry();
            if (!e) break;
            hcpy(e->name, child->name, sizeof(e->name));
            /* Derive device name from handler name:
             * "aux-handler" -> "AUX:", "port-handler" -> "PORT:" */
            int nl = hlen(child->name);
            int dl = 0;
            if (nl > 8 && hcmp(child->name + nl - 8, "-handler")) {
                /* strip "-handler" suffix */
                int base = nl - 8;
                if (base > (int)sizeof(e->device_name) - 2)
                    base = (int)sizeof(e->device_name) - 2;
                for (int i = 0; i < base; i++) {
                    char c = child->name[i];
                    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
                    e->device_name[dl++] = c;
                }
                e->device_name[dl++] = ':';
                e->device_name[dl] = '\0';
            } else {
                /* Use filename as-is with colon */
                if (nl > (int)sizeof(e->device_name) - 2)
                    nl = (int)sizeof(e->device_name) - 2;
                for (int i = 0; i < nl; i++)
                    e->device_name[dl++] = child->name[i];
                e->device_name[dl++] = ':';
                e->device_name[dl] = '\0';
            }
            e->is_running = 0;
            e->is_filesystem = 0; /* default, updated on load */
            e->handler = NULL;
            e->port = NULL;
            e->device_ref = NULL;
            e->seglist_bptr = 0;
            found++;
            kprint("[HandlerLoader] Discovered: ");
            kprint(e->name);
            kprint(" -> ");
            kprint(e->device_name);
            kprint("\n");
        }
        child = child->next_sibling;
    }

    kprint("[HandlerLoader] Scanned L:, found ");
    kprinthex(found);
    kprint(" handler(s)\n");
    return found;
}

LHandlerEntry *HandlerLoader_RegisterNative(const char *name,
                                            const char *device_name,
                                            int is_filesystem,
                                            Handler *handler)
{
    if (!name || !device_name || !handler) return NULL;

    LHandlerEntry *e = find_by_name(name);
    if (e) {
        /* Update existing entry */
        e->handler = handler;
        e->port = &handler->port;
        e->is_running = 1;
        e->is_filesystem = is_filesystem;
        return e;
    }

    e = alloc_entry();
    if (!e) return NULL;

    hcpy(e->name, name, sizeof(e->name));
    hcpy(e->device_name, device_name, sizeof(e->device_name));
    e->handler = handler;
    e->port = &handler->port;
    e->is_running = 1;
    e->is_filesystem = is_filesystem;
    e->device_ref = NULL;
    e->seglist_bptr = 0;

    kprint("[HandlerLoader] Registered native: ");
    kprint(name);
    kprint(" -> ");
    kprint(device_name);
    kprint("\n");
    return e;
}

LHandlerEntry *HandlerLoader_Load(const char *name)
{
    if (!name || !*name) return NULL;

    /* Already loaded? */
    LHandlerEntry *e = find_by_name(name);
    if (e && e->is_running) return e;

    /* Try to load from L: (TODO: M68k binary loading via LoadSeg) */
    char path[128];
    const char *resolved = VFS_ExpandAssigns("L:", path, sizeof(path));
    if (resolved) {
        int rl = hlen(resolved);
        if (rl > 0 && resolved[rl - 1] == '/') rl--;
        /* Build full path: resolved/name */
        if (rl + 1 + hlen(name) < (int)sizeof(path) - 1) {
            int p = 0;
            for (int i = 0; i < rl; i++) path[p++] = resolved[i];
            path[p++] = '/';
            for (int i = 0; name[i]; i++) path[p++] = name[i];
            path[p] = '\0';

            VfsFile fh;
            if (VFS_Open(&fh, path, VFS_READ)) {
                VFS_Close(&fh);
                /* Binary exists — for now we fall through to native
                 * built-in handler until M68k loading is wired. */
            }
        }
    }

    /* If no entry yet, create one so we can attach a native handler */
    if (!e) {
        e = alloc_entry();
        if (!e) return NULL;
        hcpy(e->name, name, sizeof(e->name));
        /* Derive device name */
        int nl = hlen(name);
        int dl = 0;
        if (nl > 8 && hcmp(name + nl - 8, "-handler")) {
            int base = nl - 8;
            if (base > (int)sizeof(e->device_name) - 2)
                base = (int)sizeof(e->device_name) - 2;
            for (int i = 0; i < base; i++) {
                char c = name[i];
                if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
                e->device_name[dl++] = c;
            }
            e->device_name[dl++] = ':';
            e->device_name[dl] = '\0';
        } else {
            if (nl > (int)sizeof(e->device_name) - 2)
                nl = (int)sizeof(e->device_name) - 2;
            for (int i = 0; i < nl; i++) e->device_name[dl++] = name[i];
            e->device_name[dl++] = ':';
            e->device_name[dl] = '\0';
        }
    }

    /* Built-in native handlers for known names */
    Handler *h = NULL;
    if (hcmp(name, "aux-handler")) {
        h = AuxHandler_Create("aux-handler");
    } else if (hcmp(name, "port-handler")) {
        h = PortHandler_Create("port-handler");
    }

    if (h) {
        e->handler = h;
        e->port = &h->port;
        e->is_running = 1;
        e->is_filesystem = 0;
        kprint("[HandlerLoader] Loaded native: ");
        kprint(name);
        kprint(" -> ");
        kprint(e->device_name);
        kprint("\n");
        return e;
    }

    /* Unknown handler and no binary — fail */
    if (!e->is_running) {
        kprint("[HandlerLoader] Failed to load: ");
        kprint(name);
        kprint("\n");
        return NULL;
    }

    return e;
}

void HandlerLoader_Unload(const char *name)
{
    if (!name) return;
    LHandlerEntry *e = find_by_name(name);
    if (!e) return;

    if (e->handler && e->handler->ProcessPacket) {
        DosPacket pkt;
        pkt.dp_Next = NULL;
        pkt.dp_Type = ACTION_DIE;
        pkt.dp_Res1 = 0;
        pkt.dp_Res2 = 0;
        e->handler->ProcessPacket(e->handler, &pkt);
    }

    e->is_running = 0;
    e->handler = NULL;
    e->port = NULL;
    e->seglist_bptr = 0;

    kprint("[HandlerLoader] Unloaded: ");
    kprint(name);
    kprint("\n");
}

LHandlerEntry *HandlerLoader_FindByDevice(const char *device_name)
{
    return find_by_device(device_name);
}

LHandlerEntry *HandlerLoader_FindByName(const char *name)
{
    return find_by_name(name);
}

int HandlerLoader_ListAll(LHandlerEntry *out[], int max_count)
{
    int n = 0;
    for (int i = 0; i < g_lhandler_count && n < max_count; i++)
        out[n++] = &g_lhandlers[i];
    return n;
}

int HandlerLoader_IsFilesystem(const char *name)
{
    LHandlerEntry *e = find_by_name(name);
    return e ? e->is_filesystem : 0;
}
