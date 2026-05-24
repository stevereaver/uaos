/* filebrowser.c — UAOS Workbench-style file browser / drawer window
 *
 * Presents a Workbench 3.x Drawer window for a named volume.
 * Contents are static (no real filesystem yet); shows placeholder entries
 * with Amiga-style small file icons drawn in the client area.
 *
 * Each volume has one window slot — double-clicking the desktop icon
 * opens it; if already open it is raised to the top.
 */

#include "filebrowser.h"
#include "wm.h"
#include "framebuffer.h"
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * String helpers (no libc)
 * ========================================================================= */

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void str_cp(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* =========================================================================
 * Static file entries per volume
 * ========================================================================= */

typedef struct { const char *name; const char *type; } FileEntry;

static const FileEntry k_ramdisk_files[] = {
    { "T",          "DIR"  },
    { "CLIPS",      "DIR"  },
    { "ENV",        "DIR"  },
    { NULL, NULL }
};

static const FileEntry k_uaos_files[] = {
    { "C",          "DIR"  },
    { "S",          "DIR"  },
    { "Libs",       "DIR"  },
    { "Devs",       "DIR"  },
    { "Prefs",      "DIR"  },
    { "System",     "DIR"  },
    { "Utilities",  "DIR"  },
    { "Shell",      "PROG" },
    { NULL, NULL }
};

/* =========================================================================
 * Per-browser instance state (one per volume)
 * ========================================================================= */

#define MAX_BROWSERS 4

typedef struct {
    char             volume[32];
    int              wm_handle;      /* -1 = not open */
    const FileEntry *entries;
    int              scroll;         /* future: vertical scroll offset */
} Browser;

static Browser g_browsers[MAX_BROWSERS];
static int     g_n_browsers = 0;

/* =========================================================================
 * Drawing helpers
 * ========================================================================= */

#define TITLEBAR_H  WM_TITLEBAR_H
#define BORDER      2
#define ICON_COL_W  96    /* horizontal cell width per icon */
#define ICON_ROW_H  52    /* vertical cell height per icon */
#define ICON_SZ     32    /* small icon bitmap size */
#define LABEL_H     14

static void draw_small_icon(int x, int y, const char *type, uint32_t col)
{
    /* Tiny folder / file icon */
    if (type[0] == 'D') {
        /* Folder tab */
        FB_FillRect(x, y + 4, ICON_SZ / 2, 5, col);
        FB_FillRect(x, y + 8, ICON_SZ, ICON_SZ - 8, col);
        FB_DrawRect(x, y + 4, ICON_SZ / 2, 5, WB_DARK_GREY);
        FB_DrawRect(x, y + 8, ICON_SZ, ICON_SZ - 8, WB_DARK_GREY);
        FB_DrawHLine(x, y + 8, ICON_SZ / 2 + 1, WB_DARK_GREY);
    } else {
        /* File / program icon */
        FB_FillRect(x, y, ICON_SZ - 6, ICON_SZ, col);
        /* Dog-ear fold */
        FB_FillRect(x + ICON_SZ - 6, y + 6, 6, ICON_SZ - 6, col);
        FB_DrawVLine(x + ICON_SZ - 6, y, 6, WB_DARK_GREY);
        FB_DrawHLine(x + ICON_SZ - 6, y + 6, 6, WB_DARK_GREY);
        FB_DrawRect(x, y, ICON_SZ - 6, ICON_SZ, WB_DARK_GREY);
        FB_DrawRect(x, y, ICON_SZ, ICON_SZ, WB_DARK_GREY);
    }
    /* White highlight */
    FB_DrawHLine(x + 1, y + (type[0]=='D' ? 9 : 1), 4, WB_WHITE);
    FB_DrawVLine(x + 1, y + (type[0]=='D' ? 9 : 1), 4, WB_WHITE);
}

/* Centred string in a fixed-width cell (clip at cell boundary) */
static void draw_label_centred(int cx, int y, int cell_w,
                                const char *s, uint32_t fg, uint32_t bg)
{
    int len = 0;
    while (s[len]) len++;
    int tx = cx + (cell_w - len * 8) / 2;
    if (tx < cx) tx = cx;
    FB_PutStr(tx, y, s, fg, bg);
}

static void browser_key(char c) { (void)c; }

/* =========================================================================
 * Per-window draw shim — each window needs its own callback to know
 * which browser it belongs to (WM callbacks don't receive a handle).
 * g_draw_ctx is set to the right Browser* before the WM calls the shim.
 * ========================================================================= */

static void browser_draw_impl(Browser *b, int wx, int wy, int ww, int wh)
{
    int cy = wy + TITLEBAR_H + BORDER;
    int ch = wh - TITLEBAR_H - BORDER * 2;
    int cx = wx + BORDER;
    int cw = ww - BORDER * 2;

    FB_FillRect(cx, cy, cw, ch, WB_GREY);
    FB_DrawRect(cx, cy, cw, ch, WB_DARK_GREY);

    if (!b) return;

    /* Path bar */
    FB_FillRect(cx + 1, cy + 1, cw - 2, 16, WB_WHITE);
    FB_PutStr(cx + 4, cy + 1, b->volume, WB_BLACK, WB_WHITE);
    FB_DrawHLine(cx + 1, cy + 17, cw - 2, WB_DARK_GREY);

    /* Icon grid */
    const FileEntry *e = b->entries;
    int col = 0, row = 0;
    int grid_y = cy + 20;
    int cols    = (cw - 8) / ICON_COL_W;
    if (cols < 1) cols = 1;

    for (int i = 0; e && e[i].name; i++) {
        int ix = cx + 4 + col * ICON_COL_W + (ICON_COL_W - ICON_SZ) / 2;
        int iy = grid_y + row * ICON_ROW_H + 4;

        if (iy + ICON_SZ + LABEL_H < cy || iy > cy + ch) {
            if (++col >= cols) { col = 0; row++; }
            continue;
        }

        uint32_t icol = (e[i].type[0] == 'D') ? WB_ORANGE : WB_BLUE;
        draw_small_icon(ix, iy, e[i].type, icol);
        draw_label_centred(cx + 4 + col * ICON_COL_W, iy + ICON_SZ + 2,
                           ICON_COL_W, e[i].name, WB_BLACK, WB_GREY);

        if (++col >= cols) { col = 0; row++; }
    }
}

static void draw_shim_0(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[0], wx, wy, ww, wh); }
static void draw_shim_1(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[1], wx, wy, ww, wh); }
static void draw_shim_2(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[2], wx, wy, ww, wh); }
static void draw_shim_3(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[3], wx, wy, ww, wh); }

typedef void (*DrawShim)(int,int,int,int);
static const DrawShim k_shims[MAX_BROWSERS] = {
    draw_shim_0, draw_shim_1, draw_shim_2, draw_shim_3
};

/* =========================================================================
 * Public API
 * ========================================================================= */

void FileBrowser_Open(const char *volume)
{
    /* Find existing browser for this volume */
    for (int i = 0; i < g_n_browsers; i++) {
        if (str_eq(g_browsers[i].volume, volume)) {
            if (g_browsers[i].wm_handle >= 0 &&
                WM_IsWindowActive(g_browsers[i].wm_handle)) {
                /* Already open — bring to front */
                WM_Redraw();
                return;
            }
            /* Was closed — fall through to re-open in the same slot */
            g_browsers[i].wm_handle = -1;
            g_n_browsers = i;   /* reclaim slot */
            break;
        }
    }

    /* Allocate new browser slot */
    if (g_n_browsers >= MAX_BROWSERS) return;
    int idx = g_n_browsers++;

    Browser *b = &g_browsers[idx];
    str_cp(b->volume, volume, 32);
    b->scroll = 0;

    /* Select file list */
    if (volume[0] == 'R')        /* "RAM Disk" */
        b->entries = k_ramdisk_files;
    else
        b->entries = k_uaos_files;

    /* Stagger windows so they don't all stack exactly */
    int wx = 80  + idx * 24;
    int wy = 60  + idx * 24;

    b->wm_handle = WM_AddWindow(wx, wy, 320, 240, volume,
                                k_shims[idx], browser_key);

    WM_Redraw();
}
