/*
 * loadable_lib.c — UAOS Loadable Library System
 *
 * Scans Workbench:LIBS/ for *.library files at boot and records each
 * name + version for the C:libs listing.  This is metadata only: real
 * hunk-format libraries are loaded on demand by exec.library/OpenLibrary
 * (it LoadSegs LIBS:<name> through the emulation layer, finds the RomTag,
 * and runs InitResident).  Version detection: read up to the first 4 KB,
 * locate a Resident tag (0x4AFC matchword, rt_Type byte at +12 == 9
 * NT_LIBRARY) and take rt_Version at +11; files without a valid RomTag
 * are recorded with version 0.
 */

#include "loadable_lib.h"
#include "dos/vfs.h"
#include "boot/kprint.h"
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
#define SCAN_BYTES  4096

typedef struct {
    char     name[MAX_NAME_LEN];
    uint16_t version;
    uint8_t  active;
} ScannedLib;

static ScannedLib g_scanned[MAX_SCANNED];
static int        g_scanned_count = 0;

#define RT_MATCHWORD_HI 0x4A
#define RT_MATCHWORD_LO 0xFC
#define NT_LIBRARY      9

/* Find the RomTag version in the first `size` bytes of a library image,
 * or 0 when there is none.  The matchword is 0x4AFC at an even offset;
 * the Resident layout puts rt_Version at +11 and rt_Type at +12. */
static uint16_t librom_version(const uint8_t *buf, uint32_t size)
{
    uint32_t off = 0;
    while (off + 13 < size) {
        if (buf[off] == RT_MATCHWORD_HI && buf[off + 1] == RT_MATCHWORD_LO &&
            buf[off + 12] == NT_LIBRARY)
            return buf[off + 11];
        off += 2;
    }
    return 0;
}

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

    static uint8_t scan_buf[SCAN_BYTES];

    int found = 0;
    while (node) {
        if (node->type == RAMFS_TYPE_FILE && ends_with(node->name, ".library")) {
            char path[128];
            scopy(path, "Workbench:LIBS/", 128);
            scat(path, node->name, 128);

            VfsFile fh;
            if (VFS_Open(&fh, path, VFS_READ)) {
                uint32_t size = VFS_Size(&fh);
                uint32_t want = size < SCAN_BYTES ? size : SCAN_BYTES;
                uint16_t version = 0;
                if (want > 0) {
                    uint32_t got = VFS_Read(&fh, scan_buf, want);
                    if (got == want)
                        version = librom_version(scan_buf, got);
                }
                VFS_Close(&fh);

                if (g_scanned_count < MAX_SCANNED) {
                    ScannedLib *s = &g_scanned[g_scanned_count++];
                    scopy(s->name, node->name, MAX_NAME_LEN);
                    s->version = version;
                    s->active = 1;
                }

                kprint("[LOADABLE] Found ");
                kprint(node->name);
                kprint(" (");
                kprintdec(size);
                kprint(" bytes) v");
                kprintdec(version);
                kprint("\n");
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
