/*
 * loadable_lib.c — UAOS Loadable Library System
 *
 * Scans Workbench:LIBS/ for *.library files at boot,
 * reads the full M68k binary, and registers it with the
 * emulation layer for installation into guest RAM.
 */

#include "loadable_lib.h"
#include "dos/vfs.h"
#include "boot/kprint.h"
#include "../../emulation/uaos_emu.h"
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Tiny freestanding helpers
 * ------------------------------------------------------------------------- */

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

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

/* -------------------------------------------------------------------------
 * Scanned library info (for C:libs listing)
 * ------------------------------------------------------------------------- */

#define MAX_SCANNED 16
#define MAX_NAME_LEN 64

typedef struct {
    char     name[MAX_NAME_LEN];
    uint16_t version;
    uint8_t  active;
} ScannedLib;

static ScannedLib g_scanned[MAX_SCANNED];
static int        g_scanned_count = 0;

/* -------------------------------------------------------------------------
 * Boot scan
 * ------------------------------------------------------------------------- */

void UAOS_LoadableLib_Init(void)
{
    kprint("[LOADABLE] Scanning LIBS: for .library files...\n");
    g_scanned_count = 0;

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

            VfsFile fh;
            if (VFS_Open(&fh, path, VFS_READ)) {
                uint32_t size = VFS_Size(&fh);
                if (size > 0 && size <= 8192) {
                    static uint8_t lib_buf[16][1024];
                    static int lib_buf_idx = 0;

                    if (lib_buf_idx < 16 && size <= 1024) {
                        uint32_t got = VFS_Read(&fh, lib_buf[lib_buf_idx], size);
                        if (got == size) {
                            uint32_t base = 0;
                            UAOS_Emu_RegisterLoadableLib(node->name,
                                lib_buf[lib_buf_idx], size, &base);

                            /* Record for C:libs listing */
                            if (g_scanned_count < MAX_SCANNED) {
                                ScannedLib *s = &g_scanned[g_scanned_count++];
                                scopy(s->name, node->name, MAX_NAME_LEN);
                                s->version = (size > 9)
                                    ? (uint16_t)(((uint16_t)lib_buf[lib_buf_idx][8] << 8)
                                                  | (uint16_t)lib_buf[lib_buf_idx][9])
                                    : 0;
                                s->active = 1;
                            }

                            kprint("[LOADABLE] Registered ");
                            kprint(node->name);
                            kprint(" (");
                            kprintdec(size);
                            kprint(" bytes) base=0x");
                            kprinthex(base);
                            kprint("\n");
                            lib_buf_idx++;
                        }
                    }
                }
                VFS_Close(&fh);
            }
            found++;
        }
        node = node->next_sibling;
    }

    if (found == 0)
        kprint("[LOADABLE] No .library files found.\n");
}

/* -------------------------------------------------------------------------
 * Listing
 * ------------------------------------------------------------------------- */

int UAOS_LoadableLib_ListAll(char *names[], uint16_t versions[], int max_count)
{
    if (!names || !versions || max_count <= 0) return g_scanned_count;
    int copy = (g_scanned_count < max_count) ? g_scanned_count : max_count;
    for (int i = 0; i < copy; i++) {
        names[i]    = g_scanned[i].name;
        versions[i] = g_scanned[i].version;
    }
    return g_scanned_count;
}
