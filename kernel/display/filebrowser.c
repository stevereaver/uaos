/* filebrowser.c — UAOS Workbench-style file browser / drawer window
 *
 * Presents a Workbench 3.x Drawer window for a named volume.
 * with Amiga-style small file icons drawn in the client area.
 *
 * Each volume has one window slot — double-clicking the desktop icon
 * opens it; if already open it is raised to the top.
 */

#include "filebrowser.h"
#include "wm.h"
#include "framebuffer.h"
#include "desktop.h"
#include "calc_win.h"
#include "clock_win.h"
#include "pointer_prefs.h"
#include "../dos/blockdev.h"
#include "../dos/vfs.h"
#include "../dos/ramfs.h"
#include <stdint.h>
#include <stddef.h>

/* Debug output - prints to screen for debugging */
#define FB_DEBUG 1
#if FB_DEBUG
    #define FB_LOG(msg) do { extern void kprint(const char *); kprint(msg); } while(0)
    #define FB_LOG_DEC(v) do { extern void kprintdec(uint32_t); kprintdec((uint32_t)(v)); } while(0)

    /* On-screen debug output */
    #define DBG_MAX_LINES 8
    #define DBG_LINE_LEN 60
    static char dbg_lines[DBG_MAX_LINES][DBG_LINE_LEN];
    static int dbg_line_count = 0;
    static int dbg_scroll = 0;

    void dbg_add_line(const char *msg)
    {
        if (dbg_line_count < DBG_MAX_LINES) {
            int i = 0;
            while (i < DBG_LINE_LEN - 1 && msg[i]) {
                dbg_lines[dbg_line_count][i] = msg[i];
                i++;
            }
            dbg_lines[dbg_line_count][i] = '\0';
            dbg_line_count++;
        } else {
            /* Scroll up */
            for (int i = 0; i < DBG_MAX_LINES - 1; i++) {
                int j = 0;
                while (j < DBG_LINE_LEN) {
                    dbg_lines[i][j] = dbg_lines[i+1][j];
                    j++;
                }
            }
            int i = 0;
            while (i < DBG_LINE_LEN - 1 && msg[i]) {
                dbg_lines[DBG_MAX_LINES-1][i] = msg[i];
                i++;
            }
            dbg_lines[DBG_MAX_LINES-1][i] = '\0';
        }
    }

    static void dbg_draw(void)
    {
        extern void FB_FillRect(int x, int y, int w, int h, uint32_t colour);
        extern void FB_DrawRect(int x, int y, int w, int h, uint32_t colour);
        extern void FB_PutStr(int x, int y, const char *s, uint32_t fg, uint32_t bg);

        int x = 10, y = 400;
        int w = 480, h = DBG_MAX_LINES * 12 + 8;

        /* Dark background (0x202020) and red border (0xFF0000) */
        FB_FillRect(x, y, w, h, 0x00202020U);
        FB_DrawRect(x, y, w, h, 0x00FF0000U);

        for (int i = 0; i < dbg_line_count; i++) {
            FB_PutStr(x + 4, y + 4 + i * 12, dbg_lines[i], 0x00FFFF00U, 0x00202020U);
        }
    }

    #define FB_LOG_SCREEN(msg) dbg_add_line(msg)
#else
    #define FB_LOG(msg) do {} while(0)
    #define FB_LOG_DEC(v) do {} while(0)
    #define FB_LOG_SCREEN(msg) do {} while(0)
    static void dbg_draw(void) {}
#endif

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

static int str_len(const char *s)
{
    int n = 0; while (s[n]) n++; return n;
}

/* Compute the parent path of a volume string into dst[max].
 * Returns 1 if a parent exists, 0 if already at a root volume. */
static int parent_path(const char *vol, char *dst, int max)
{
    /* Root volumes have no parent: "RAM Disk" (no colon) or "UAOS:" (colon
     * is the last character, no slash after it). */
    int len = str_len(vol);

    /* Find the last '/' */
    int last_slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (vol[i] == '/') { last_slash = i; break; }
    }

    if (last_slash <= 0) return 0;  /* no slash — root volume */

    /* Strip everything from the last '/' onward */
    int plen = last_slash;          /* e.g. "UAOS:" has len 5, slash at 5 */

    /* Special case: if the result would end in ':' it is a root volume
     * key like "UAOS:" — keep it as-is. */
    /* If the result ends in a plain name with no ':', it is also a valid
     * root key (e.g. "RAM Disk"). */
    if (plen <= 0) return 0;

    if (plen >= max) plen = max - 1;
    for (int i = 0; i < plen; i++) dst[i] = vol[i];
    dst[plen] = '\0';
    return 1;
}

/* =========================================================================
 * File entry structure and VFS integration
 * ========================================================================= */

typedef struct { const char *name; const char *type; } FileEntry;

/* Placeholder for partition volumes until FAT32_ReadDir is implemented */
static const FileEntry k_partition_empty_files[] = { { NULL, NULL } };

/* Per-browser entry storage to prevent shared buffer corruption */
#define MAX_BROWSER_ENTRIES 32
#define MAX_ENTRY_NAME_LEN 16
#define MAX_ENTRY_TYPE_LEN 8

typedef struct {
    FileEntry entries[MAX_BROWSER_ENTRIES + 1];
    char names[MAX_BROWSER_ENTRIES][MAX_ENTRY_NAME_LEN];
    char types[MAX_BROWSER_ENTRIES][MAX_ENTRY_TYPE_LEN];
} BrowserEntryBuffer;

/* Load entries from path into a specific browser's buffer */
static const FileEntry *load_entries_for_browser(const char *path, BrowserEntryBuffer *buf)
{
    if (!buf) return NULL;

    /* Try VFS first — if the path resolves to a mounted volume, enumerate it */
    RamFsNode *child = VFS_OpenDir(path);
    if (child) {
        int n = 0;
        while (child && n < MAX_BROWSER_ENTRIES) {
            int ni = 0;
            while (ni < MAX_ENTRY_NAME_LEN - 1 && child->name[ni]) {
                buf->names[n][ni] = child->name[ni];
                ni++;
            }
            buf->names[n][ni] = '\0';
            buf->entries[n].name = buf->names[n];

            if (child->type == RAMFS_TYPE_DIR) {
                buf->types[n][0] = 'D'; buf->types[n][1] = 'I';
                buf->types[n][2] = 'R'; buf->types[n][3] = '\0';
            } else {
                buf->types[n][0] = 'F'; buf->types[n][1] = 'I';
                buf->types[n][2] = 'L'; buf->types[n][3] = 'E';
                buf->types[n][4] = '\0';
            }
            buf->entries[n].type = buf->types[n];
            n++;
            child = child->next_sibling;
        }
        buf->entries[n].name = NULL;
        buf->entries[n].type = NULL;
        return buf->entries;
    }

    /* Check if this is a partition device (not yet mounted in VFS) */
    BlockDev *dev = BlockDev_Find(path);
    if (!dev) {
        BlockDev *all = BlockDev_GetList();
        while (all) {
            if (all->display_name && str_eq(all->display_name, path)) {
                dev = all;
                break;
            }
            all = all->next;
        }
    }
    if (dev && dev->part_offset != 0) {
        return k_partition_empty_files;
    }

    return NULL;  /* no hardcoded fallback - use real filesystem only */
}

/* =========================================================================
 * Per-browser instance state (one per volume)
 * ========================================================================= */

#define MAX_BROWSERS 16
#define DBLCLICK_TICKS 2

typedef struct {
    char             volume[32];     /* path string used as key and title */
    int              wm_handle;      /* -1 = not open */
    const FileEntry *entries;
    BrowserEntryBuffer entry_buffer; /* Private buffer for this browser's entries */
    int              scroll;         /* future: vertical scroll offset */
    /* Double-click tracking for icon cells */
    int              last_click_icon; /* index of last clicked icon, -1 = none */
    unsigned int     last_click_tick;
    /* Cached window geometry (updated every draw call) */
    int              win_x, win_y, win_w, win_h;
    /* Icon drag state */
    int              drag_icon;       /* -1 = none, >=0 = entry index being dragged */
    int              drag_active;     /* 1 = dragging visually (threshold passed) */
    int              drag_off_x, drag_off_y; /* offset from icon top-left at press */
    int              drag_x, drag_y;  /* current screen position of dragged icon */
    int              drag_start_x, drag_start_y; /* mouse position at press */
} Browser;

static Browser g_browsers[MAX_BROWSERS];
static int     g_n_browsers = 0;

/* Forward declaration — click shims call this */
static void browser_click_impl(Browser *b, int wh, int mx, int my);

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

static void browser_key(char c)
{
    if (c != 27) return;  /* ESC */
    int fh = WM_GetFocus();
    if (fh < 0) return;
    /* Find the browser that owns this focused window and close it */
    for (int i = 0; i < MAX_BROWSERS; i++) {
        if (g_browsers[i].wm_handle == fh) {
            WM_CloseWindow(fh);
            g_browsers[i].wm_handle = -1;
            g_browsers[i].volume[0] = '\0';
            g_browsers[i].drag_icon   = -1;
            g_browsers[i].drag_active = 0;
            return;
        }
    }
}

/* Compute the icon index hit by client-relative pixel (rx, ry).          */
/* Returns -1 if no icon was hit. Replicates layout from browser_draw_impl */
static int browser_icon_hit(Browser *b, int wx, int wy, int ww, int wh,
                             int mx, int my)
{
    int cx = wx + 1;
    int cy = wy + WM_TITLEBAR_H;
    int cw = ww - 1 - WM_SCROLLBAR_W;
    int ch = wh - WM_TITLEBAR_H - WM_SCROLLBAR_W;

    /* Must be inside client area */
    if (mx < cx || mx >= cx + cw || my < cy || my >= cy + ch) return -1;

    int n_entries = 0;
    const FileEntry *e = b->entries;
    while (e && e[n_entries].name) n_entries++;

    int usable = cw - 8;
    int cols   = usable / ICON_COL_W;
    if (cols < 1) cols = 1;
    int cell_w = (cols == 1) ? usable : ICON_COL_W;

    int path_h  = 20;
    int scroll_y = (b->wm_handle >= 0) ? WM_GetScrollY(b->wm_handle) : 0;
    int grid_base = cy + path_h - scroll_y;

    /* Path bar absorbs clicks in its region */
    if (my < cy + path_h) return -1;

    int col = 0, row = 0;
    for (int i = 0; e && e[i].name; i++) {
        int cell_x = cx + 4 + col * cell_w;
        int iy     = grid_base + row * ICON_ROW_H;
        /* Cell bounds: full cell_w wide, ICON_ROW_H tall */
        if (mx >= cell_x && mx < cell_x + cell_w &&
            my >= iy     && my < iy + ICON_ROW_H)
            return i;
        if (++col >= cols) { col = 0; row++; }
    }
    return -1;
}

/* Compute the grid cell index at screen pixel (mx, my).
 * Returns -1 if outside the grid.  Does NOT account for a dragged icon. */
static int browser_cell_at_pos(Browser *b, int wx, int wy, int ww, int wh,
                                int mx, int my)
{
    int cx = wx + 1;
    int cy = wy + WM_TITLEBAR_H;
    int cw = ww - 1 - WM_SCROLLBAR_W;
    int ch = wh - WM_TITLEBAR_H - WM_SCROLLBAR_W;

    if (mx < cx || mx >= cx + cw || my < cy || my >= cy + ch) return -1;

    int n_entries = 0;
    const FileEntry *e = b->entries;
    while (e && e[n_entries].name) n_entries++;

    int usable = cw - 8;
    int cols   = usable / ICON_COL_W;
    if (cols < 1) cols = 1;
    int cell_w = (cols == 1) ? usable : ICON_COL_W;

    int path_h = 20;
    int scroll_y = (b->wm_handle >= 0) ? WM_GetScrollY(b->wm_handle) : 0;
    int grid_base = cy + path_h - scroll_y;

    if (my < cy + path_h) return -1;

    int col = (mx - (cx + 4)) / cell_w;
    int row = (my - grid_base) / ICON_ROW_H;
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    if (col >= cols) col = cols - 1;

    int idx = row * cols + col;
    if (idx >= n_entries) return -1;
    return idx;
}

/* Called by WM on client-area click for a browser window */
static void browser_click_impl(Browser *b, int wh, int mx, int my)
{
    if (!b || b->wm_handle != wh) return;

    /* Check path-bar up-button first (sits above the icon grid) */
    int cx  = b->win_x + 1;
    int cy  = b->win_y + WM_TITLEBAR_H;
    int cw  = b->win_w - 1 - WM_SCROLLBAR_W;
    int path_h = 20;

    if (my >= cy && my < cy + path_h) {
        /* Click in path bar — check up-button hit zone */
        int ub_w = 18;
        int ub_x = cx + cw - ub_w - 2;
        int ub_y = cy + 2;
        int ub_h = path_h - 4;
        char par[64];
        if (mx >= ub_x && mx < ub_x + ub_w &&
            my >= ub_y && my < ub_y + ub_h &&
            parent_path(b->volume, par, 64)) {
            FileBrowser_Open(par);
        }
        b->last_click_icon = -1;
        b->drag_icon = -1;
        return;
    }

    int icon = browser_icon_hit(b, b->win_x, b->win_y, b->win_w, b->win_h,
                                mx, my);
    if (icon < 0) {
        /* Missed all icons — reset double-click state and cancel drag */
        b->last_click_icon = -1;
        b->drag_icon = -1;
        return;
    }

    unsigned int now = Desktop_GetTick();

    if (b->last_click_icon == icon &&
        (now - b->last_click_tick) <= DBLCLICK_TICKS) {
        /* Double-click: open folder (DIR) or launch app (PROG) */
        b->last_click_icon = -1;
        b->drag_icon = -1;
        const FileEntry *e = b->entries;
        if (e && e[icon].name && e[icon].type[0] == 'P') {
            /* Launch known applications by name (PROG type) */
            const char *nm = e[icon].name;
            int ni = 0;
            char name[32];
            while (ni < 31 && nm[ni]) { name[ni] = nm[ni]; ni++; }
            name[ni] = '\0';
            if (str_eq(name, "Calculator")) CalcWin_Open();
            else if (str_eq(name, "Clock"))       ClockWin_Open();
            else if (str_eq(name, "Pointer")) PointerPrefs_Show();
        } else if (e && e[icon].name && e[icon].type[0] == 'F') {
            /* Launch known applications by name (FILE type - VFS files) */
            const char *nm = e[icon].name;
            int ni = 0;
            char name[32];
            while (ni < 31 && nm[ni]) { name[ni] = nm[ni]; ni++; }
            name[ni] = '\0';
            if (str_eq(name, "Calculator")) CalcWin_Open();
            else if (str_eq(name, "Clock"))       ClockWin_Open();
            else if (str_eq(name, "Pointer")) PointerPrefs_Show();
        } else if (e && e[icon].name && e[icon].type[0] == 'D') {
            /* Check if this is a top-level assign directory (C, DEVS, L, LIBS, S, SYS, Tools) */
            const char *nm = e[icon].name;
            int is_top_level_assign = 0;
            if (str_eq(nm, "C") || str_eq(nm, "DEVS") || str_eq(nm, "L") || 
                str_eq(nm, "LIBS") || str_eq(nm, "S") || str_eq(nm, "SYS") ||
                str_eq(nm, "Tools")) {
                is_top_level_assign = 1;
            }
            
            char child_path[64];
            if (is_top_level_assign) {
                /* For top-level assigns, construct "Workbench:ASSIGN" directly */
                int vi = 0;
                /* Copy base volume up to colon (e.g., "Workbench:") */
                while (vi < 63 && b->volume[vi] && b->volume[vi] != ':') { 
                    child_path[vi] = b->volume[vi]; vi++; 
                }
                if (vi < 63 && b->volume[vi] == ':') {
                    child_path[vi++] = ':';
                }
                /* Add assign name */
                while (vi < 63 && *nm) { child_path[vi++] = *nm++; }
                child_path[vi] = '\0';
            } else {
                /* Build child path to match k_path_table keys:
                 *   "UAOS:"        + "/" + "C"         -> "UAOS:/C"
                 *   "RAM Disk"     + ":/" + "T"         -> "RAM Disk:/T"
                 *   "UAOS:/Prefs"  + "/" + "Env-Archive" -> "UAOS:/Prefs/Env-Archive"
                 * Rule: if volume ends in ':', append '/'; else append ":/". */
                int vi = 0;
                while (vi < 63 && b->volume[vi]) { child_path[vi] = b->volume[vi]; vi++; }
                int last = (vi > 0) ? b->volume[vi - 1] : 0;
                if (last != ':' && last != '/') {
                    /* Plain name like "RAM Disk" — add ":/" separator */
                    if (vi < 63) child_path[vi++] = ':';
                }
                if (vi < 63) child_path[vi++] = '/';
                while (vi < 63 && *nm) { child_path[vi++] = *nm++; }
                child_path[vi] = '\0';
            }
            FileBrowser_Open(child_path);
        }
    } else {
        /* First click — record for double-click detection and start drag */
        b->last_click_icon = icon;
        b->last_click_tick = now;

        int n_entries = 0;
        while (b->entries && b->entries[n_entries].name) n_entries++;
        int usable = cw - 8;
        int cols   = usable / ICON_COL_W;
        if (cols < 1) cols = 1;
        int cell_w = (cols == 1) ? usable : ICON_COL_W;
        int scroll_y = (b->wm_handle >= 0) ? WM_GetScrollY(b->wm_handle) : 0;
        int grid_base = cy + path_h - scroll_y;

        int drag_col = icon % cols;
        int drag_row = icon / cols;
        int cell_x   = cx + 4 + drag_col * cell_w;
        int iy       = grid_base + drag_row * ICON_ROW_H;
        int ix       = cell_x + (cell_w - ICON_SZ) / 2;

        b->drag_icon     = icon;
        b->drag_active   = 0;
        b->drag_off_x    = mx - ix;
        b->drag_off_y    = my - iy;
        b->drag_x        = ix;
        b->drag_y        = iy;
        b->drag_start_x  = mx;
        b->drag_start_y  = my;
    }
}

static void browser_mouse_move(int wh, int mx, int my)
{
    for (int i = 0; i < MAX_BROWSERS; i++) {
        Browser *b = &g_browsers[i];
        if (b->wm_handle == wh && b->drag_icon >= 0) {
            if (!b->drag_active) {
                int dx = mx - b->drag_start_x;
                int dy = my - b->drag_start_y;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                if (dx < 4 && dy < 4) return; /* below threshold, no redraw */
                b->drag_active = 1;
            }
            b->drag_x = mx - b->drag_off_x;
            b->drag_y = my - b->drag_off_y;
            WM_Redraw();
            return;
        }
    }
}

static void browser_mouse_release(int wh, int mx, int my)
{
    for (int i = 0; i < MAX_BROWSERS; i++) {
        Browser *b = &g_browsers[i];
        if (b->wm_handle == wh && b->drag_icon >= 0) {
            int target = browser_cell_at_pos(b, b->win_x, b->win_y,
                                              b->win_w, b->win_h, mx, my);
            int n_entries = 0;
            while (b->entries && b->entries[n_entries].name) n_entries++;
            if (target >= 0 && target != b->drag_icon && target < n_entries) {
                FileEntry tmp = b->entry_buffer.entries[b->drag_icon];
                b->entry_buffer.entries[b->drag_icon] =
                    b->entry_buffer.entries[target];
                b->entry_buffer.entries[target] = tmp;
            }
            b->drag_icon   = -1;
            b->drag_active = 0;
            WM_Redraw();
            return;
        }
    }
}

/* =========================================================================
 * Per-window draw shim — each window needs its own callback to know
 * which browser it belongs to (WM callbacks don't receive a handle).
 * g_draw_ctx is set to the right Browser* before the WM calls the shim.
 * ========================================================================= */

static void browser_draw_impl(Browser *b, int wx, int wy, int ww, int wh)
{
    /* Cache geometry so the click handler can use it without a separate API */
    if (b) { b->win_x = wx; b->win_y = wy; b->win_w = ww; b->win_h = wh; }

    /* Client area: 1px left outline, right = scrollbar, bottom = scrollbar */
    int cx = wx + 1;
    int cy = wy + WM_TITLEBAR_H;
    int cw = ww - 1 - WM_SCROLLBAR_W;
    int ch = wh - WM_TITLEBAR_H - WM_SCROLLBAR_W;

    FB_FillRect(cx, cy, cw, ch, WB_GREY);

    if (!b) return;

    /* Count entries to compute total content height */
    int n_entries = 0;
    const FileEntry *e = b->entries;
    while (e && e[n_entries].name) n_entries++;

    int usable = cw - 8;
    int cols   = usable / ICON_COL_W;
    if (cols < 1) cols = 1;
    int cell_w = (cols == 1) ? usable : ICON_COL_W;
    int n_rows = (n_entries + cols - 1) / cols;

    /* Path bar height */
    int path_h = 20;

    /* Total content height: path bar + icon rows */
    int total_h = path_h + n_rows * ICON_ROW_H + 8;

    /* Tell WM the content size for proportional scrollbar thumb */
    if (b->wm_handle >= 0)
        WM_SetScrollInfo(b->wm_handle, cw, total_h);

    /* Get current vertical scroll offset */
    int scroll_y = (b->wm_handle >= 0) ? WM_GetScrollY(b->wm_handle) : 0;

    /* Icon grid — offset by scroll_y, strictly clipped to client area */
    int icon_top    = cy + path_h;   /* first pixel icons may appear */
    int icon_bottom = cy + ch - 1;   /* last pixel inside window (1px outline) */
    int grid_base = icon_top - scroll_y;

    for (int i = 0; e && e[i].name; i++) {
        if (b->drag_active && b->drag_icon == i)
            continue; /* dragged icon drawn separately — only hide once drag is active */

        int col       = i % cols;
        int row       = i / cols;
        int cell_x    = cx + 4 + col * cell_w;
        int iy        = grid_base + row * ICON_ROW_H;
        int ix        = cell_x + (cell_w - ICON_SZ) / 2;
        int icon_top1 = iy + 4;
        int label_bot = iy + 4 + ICON_SZ + LABEL_H + 2;

        /* Draw only when the entire icon+label fits within the clip zone */
        if (icon_top1 >= icon_top && label_bot <= icon_bottom) {
            if (ix >= cx && cell_x + cell_w <= cx + cw) {
                uint32_t icol = (e[i].type[0] == 'D') ? WB_ORANGE : WB_BLUE;
                draw_small_icon(ix, icon_top1, e[i].type, icol);
                draw_label_centred(cell_x, iy + 4 + ICON_SZ + 2,
                                   cell_w, e[i].name, WB_BLACK, WB_GREY);
            }
        }
    }

    /* Draw dragged icon at its current mouse position (only once threshold passed) */
    if (b->drag_active && b->drag_icon >= 0 && e && e[b->drag_icon].name) {
        /* Total height: 4 (gap) + ICON_SZ (icon) + 2 + LABEL_H (label) */
        int drag_h = 4 + ICON_SZ + 2 + LABEL_H;
        int dy_min = cy + path_h;
        int dy_max = cy + ch - drag_h;
        /* Only draw if the client area can actually fit the icon */
        if (dy_max >= dy_min) {
            int dx = b->drag_x;
            int dy = b->drag_y;
            if (dx < cx) dx = cx;
            if (dx + ICON_SZ > cx + cw) dx = cx + cw - ICON_SZ;
            if (dy < dy_min) dy = dy_min;
            if (dy > dy_max) dy = dy_max;
            uint32_t icol = (e[b->drag_icon].type[0] == 'D') ? WB_ORANGE : WB_BLUE;
            draw_small_icon(dx, dy + 4, e[b->drag_icon].type, icol);
            draw_label_centred(dx, dy + 4 + ICON_SZ + 2, ICON_SZ,
                               e[b->drag_icon].name, WB_BLACK, WB_GREY);
        }
    }

    /* Path bar drawn last so it always appears on top of any icon overflow */
    FB_FillRect(cx, cy, cw, path_h, WB_WHITE);
    FB_PutStr(cx + 4, cy + 2, b->volume, WB_BLACK, WB_WHITE);

    /* Up-button: small arrow box on the right side of the path bar,
     * only shown when a parent directory exists */
    char par[64];
    if (parent_path(b->volume, par, 64)) {
        int ub_w = 18;
        int ub_x = cx + cw - ub_w - 2;
        int ub_y = cy + 2;
        int ub_h = path_h - 4;
        FB_FillRect(ub_x, ub_y, ub_w, ub_h, WB_LIGHT_GREY);
        FB_DrawRect(ub_x, ub_y, ub_w, ub_h, WB_DARK_GREY);
        /* Draw a small upward triangle (tip at top, base at bottom):
         * row 0 = tip (1px wide) at ty+0, row 4 = base (9px wide) at ty+4 */
        int mx2 = ub_x + ub_w / 2;
        int ty  = ub_y + (ub_h - 5) / 2;
        for (int row = 0; row < 5; row++)
            FB_DrawHLine(mx2 - (4 - row), ty + (4 - row), (4 - row) * 2 + 1, WB_DARK_GREY);
    }

    FB_DrawHLine(cx, cy + path_h - 1, cw, WB_DARK_GREY);

#if FB_DEBUG
    /* Draw debug overlay on screen */
    dbg_draw();
#endif
}

static void draw_shim_0(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[0], wx, wy, ww, wh); }
static void draw_shim_1(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[1], wx, wy, ww, wh); }
static void draw_shim_2(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[2], wx, wy, ww, wh); }
static void draw_shim_3(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[3], wx, wy, ww, wh); }
static void draw_shim_4(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[4], wx, wy, ww, wh); }
static void draw_shim_5(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[5], wx, wy, ww, wh); }
static void draw_shim_6(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[6], wx, wy, ww, wh); }
static void draw_shim_7(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[7], wx, wy, ww, wh); }
static void draw_shim_8(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[8], wx, wy, ww, wh); }
static void draw_shim_9(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[9], wx, wy, ww, wh); }
static void draw_shim_10(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[10], wx, wy, ww, wh); }
static void draw_shim_11(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[11], wx, wy, ww, wh); }
static void draw_shim_12(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[12], wx, wy, ww, wh); }
static void draw_shim_13(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[13], wx, wy, ww, wh); }
static void draw_shim_14(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[14], wx, wy, ww, wh); }
static void draw_shim_15(int wx, int wy, int ww, int wh) { browser_draw_impl(&g_browsers[15], wx, wy, ww, wh); }

static void click_shim_0(int wh, int mx, int my) { browser_click_impl(&g_browsers[0], wh, mx, my); }
static void click_shim_1(int wh, int mx, int my) { browser_click_impl(&g_browsers[1], wh, mx, my); }
static void click_shim_2(int wh, int mx, int my) { browser_click_impl(&g_browsers[2], wh, mx, my); }
static void click_shim_3(int wh, int mx, int my) { browser_click_impl(&g_browsers[3], wh, mx, my); }
static void click_shim_4(int wh, int mx, int my) { browser_click_impl(&g_browsers[4], wh, mx, my); }
static void click_shim_5(int wh, int mx, int my) { browser_click_impl(&g_browsers[5], wh, mx, my); }
static void click_shim_6(int wh, int mx, int my) { browser_click_impl(&g_browsers[6], wh, mx, my); }
static void click_shim_7(int wh, int mx, int my) { browser_click_impl(&g_browsers[7], wh, mx, my); }
static void click_shim_8(int wh, int mx, int my) { browser_click_impl(&g_browsers[8], wh, mx, my); }
static void click_shim_9(int wh, int mx, int my) { browser_click_impl(&g_browsers[9], wh, mx, my); }
static void click_shim_10(int wh, int mx, int my) { browser_click_impl(&g_browsers[10], wh, mx, my); }
static void click_shim_11(int wh, int mx, int my) { browser_click_impl(&g_browsers[11], wh, mx, my); }
static void click_shim_12(int wh, int mx, int my) { browser_click_impl(&g_browsers[12], wh, mx, my); }
static void click_shim_13(int wh, int mx, int my) { browser_click_impl(&g_browsers[13], wh, mx, my); }
static void click_shim_14(int wh, int mx, int my) { browser_click_impl(&g_browsers[14], wh, mx, my); }
static void click_shim_15(int wh, int mx, int my) { browser_click_impl(&g_browsers[15], wh, mx, my); }

typedef void (*DrawShim)(int,int,int,int);
static const DrawShim k_draw_shims[MAX_BROWSERS] = {
    draw_shim_0, draw_shim_1, draw_shim_2, draw_shim_3,
    draw_shim_4, draw_shim_5, draw_shim_6, draw_shim_7,
    draw_shim_8, draw_shim_9, draw_shim_10, draw_shim_11,
    draw_shim_12, draw_shim_13, draw_shim_14, draw_shim_15
};

typedef void (*ClickShim)(int,int,int);
static const ClickShim k_click_shims[MAX_BROWSERS] = {
    click_shim_0, click_shim_1, click_shim_2, click_shim_3,
    click_shim_4, click_shim_5, click_shim_6, click_shim_7,
    click_shim_8, click_shim_9, click_shim_10, click_shim_11,
    click_shim_12, click_shim_13, click_shim_14, click_shim_15
};

/* =========================================================================
 * Public API
 * ========================================================================= */

void FileBrowser_CancelClicks(int wm_handle_keep)
{
    /* Reset pending double-click state for every browser whose WM window
     * handle is not wm_handle_keep.  Pass -1 to cancel all slots.
     * Called on every WM btn_pressed that hits a window so that a stale
     * first-click in a background browser cannot spuriously complete as a
     * double-click when a later unrelated click happens to land there. */
    for (int i = 0; i < MAX_BROWSERS; i++) {
        if (g_browsers[i].wm_handle != wm_handle_keep) {
            g_browsers[i].last_click_icon = -1;
            g_browsers[i].last_click_tick = 0;
        }
    }
}

void FileBrowser_Open(const char *volume)
{
    /* Defensive clamp in case previous buggy code corrupted the counter */
    if (g_n_browsers < 0) g_n_browsers = 0;
    if (g_n_browsers > MAX_BROWSERS) g_n_browsers = MAX_BROWSERS;

    /* Dump WM state */
    FB_LOG_SCREEN("WM:");
    for (int w = 0; w < WM_MAX_WINDOWS; w++) {
        if (WM_IsWindowActive(w)) {
            char wmst[16];
            wmst[0] = '0' + w;
            wmst[1] = 'A';
            wmst[2] = '\0';
            FB_LOG_SCREEN(wmst);
        }
    }
    /* Clear summary of all browser slots */
    FB_LOG_SCREEN("SLOTS:");
    for (int s = 0; s < g_n_browsers && s < MAX_BROWSERS; s++) {
        char sum[32];
        int si = 0;
        sum[si++] = '0' + s;
        sum[si++] = ':';
        const char *sv = g_browsers[s].volume;
        while (*sv && si < 20) sum[si++] = *sv++;
        sum[si++] = 'h';
        if (g_browsers[s].wm_handle < 0) sum[si++] = '-';
        else sum[si++] = '0' + g_browsers[s].wm_handle;
        /* Check if WM thinks this handle is active */
        int wma = WM_IsWindowActive(g_browsers[s].wm_handle);
        sum[si++] = wma ? 'A' : 'X';
        sum[si] = '\0';
        FB_LOG_SCREEN(sum);
    }
    FB_LOG("[FB] Current browsers: "); FB_LOG_DEC(g_n_browsers); FB_LOG("\n");

    /* Find existing browser for this exact volume path */
    FB_LOG("[FB] Scanning "); FB_LOG_DEC(g_n_browsers); FB_LOG(" browsers for '"); FB_LOG(volume); FB_LOG("'\n");
    char dbg2[64];
    str_cp(dbg2, "Scanning ", 64);
    int di = str_len(dbg2);
    dbg2[di++] = '0' + g_n_browsers;
    str_cp(&dbg2[di], " slots for ", 64-di);
    di = str_len(dbg2);
    const char *vp = volume;
    while (*vp && di < 60) dbg2[di++] = *vp++;
    dbg2[di] = '\0';
    FB_LOG_SCREEN(dbg2);

    /* Debug: show all slots */
    int slots_at_start = g_n_browsers;
    for (int k = 0; k < slots_at_start && k < MAX_BROWSERS; k++) {
        char dslot[64];
        str_cp(dslot, "All slot ", 64);
        int dk = str_len(dslot);
        dslot[dk++] = '0' + k;
        dslot[dk++] = '=';
        const char *bv = g_browsers[k].volume;
        while (*bv && dk < 40) dslot[dk++] = *bv++;
        dslot[dk++] = 'h';
        int h = g_browsers[k].wm_handle;
        if (h < 0) { dslot[dk++] = '-'; }
        else { dslot[dk++] = '0' + h; }
        dslot[dk] = '\0';
        FB_LOG_SCREEN(dslot);
    }
    if (g_n_browsers != slots_at_start) {
        char dbgc[64];
        str_cp(dbgc, "COUNT CHANGED! was ", 64);
        int dc = str_len(dbgc);
        dbgc[dc++] = '0' + slots_at_start;
        str_cp(&dbgc[dc], " now ", 64-dc);
        dc = str_len(dbgc);
        dbgc[dc++] = '0' + g_n_browsers;
        dbgc[dc] = '\0';
        FB_LOG_SCREEN(dbgc);
    }

    for (int i = 0; i < slots_at_start && i < MAX_BROWSERS; i++) {
        FB_LOG("[FB] Slot "); FB_LOG_DEC(i); FB_LOG(": vol='");
        FB_LOG(g_browsers[i].volume); FB_LOG("' handle="); FB_LOG_DEC(g_browsers[i].wm_handle);
        FB_LOG(" active="); FB_LOG_DEC(WM_IsWindowActive(g_browsers[i].wm_handle));
        FB_LOG(" compare="); FB_LOG_DEC(str_eq(g_browsers[i].volume, volume));
        FB_LOG("\n");
        int match = str_eq(g_browsers[i].volume, volume);
        char dbg3[64];
        str_cp(dbg3, "Slot ", 64);
        int dj = str_len(dbg3);
        dbg3[dj++] = '0' + i;
        dbg3[dj++] = '=';
        const char *bv = g_browsers[i].volume;
        while (*bv && dj < 40) dbg3[dj++] = *bv++;
        dbg3[dj++] = 'h';
        int h = g_browsers[i].wm_handle;
        if (h < 0) { dbg3[dj++] = '-'; }
        else { dbg3[dj++] = '0' + h; }
        dbg3[dj++] = match ? '!' : '?';
        dbg3[dj] = '\0';
        FB_LOG_SCREEN(dbg3);

        if (match) {
            FB_LOG("[FB] Match found at slot "); FB_LOG_DEC(i); FB_LOG("\n");
            int active = WM_IsWindowActive(g_browsers[i].wm_handle);
            char dbgm[64];
            str_cp(dbgm, "Match slot ", 64);
            int dm = str_len(dbgm);
            dbgm[dm++] = '0' + i;
            dbgm[dm++] = ' ';
            dbgm[dm++] = 'h';
            if (g_browsers[i].wm_handle < 0) dbgm[dm++] = '-';
            else dbgm[dm++] = '0' + g_browsers[i].wm_handle;
            str_cp(&dbgm[dm], active ? " active" : " CLOSED", 64-dm);
            dm = str_len(dbgm);
            dbgm[dm] = '\0';
            FB_LOG_SCREEN(dbgm);
            if (g_browsers[i].wm_handle >= 0 && active) {
                /* Already open — raise to front and focus */
                FB_LOG_SCREEN("MATCH: raising window");
                FB_LOG("[FB] Window active, raising\n");
                WM_RaiseWindow(g_browsers[i].wm_handle);
                WM_Redraw();
                return;
            }
            /* Was closed — reuse this slot */
            FB_LOG("[FB] Reusing closed slot\n");
            FB_LOG_SCREEN("Reusing closed slot");
            Browser *b = &g_browsers[i];
            b->scroll = 0;
            b->last_click_icon = -1;
            b->last_click_tick = 0;
            b->win_x = b->win_y = b->win_w = b->win_h = 0;
            b->entries = load_entries_for_browser(volume, &b->entry_buffer);

            /* Calculate window position with stagger, clamped to desktop bounds */
            int wx = 80 + i * 120;
            int wy = 60 + i * 100;
            /* Desktop bounds: below menubar (20), above statusbar (18), window size 320x240 */
            int max_x = (int)g_fb.width - 320;
            int max_y = (int)g_fb.height - 20 - 18 - 240;  /* menubar + statusbar + window */
            if (wx > max_x) wx = max_x;
            if (wx < 0) wx = 0;
            if (wy > max_y) wy = max_y;
            if (wy < 20) wy = 20;  /* MENUBAR_H */
            int reuse_handle = WM_AddWindow(wx, wy, 320, 240, volume,
                                            k_draw_shims[i], browser_key);
            /* CRITICAL FIX: Check if another browser slot already has this handle.
             * This happens when that slot's window was closed, but it still
             * has the stale handle value. Invalidate it to prevent duplicates. */
            for (int check = 0; check < MAX_BROWSERS; check++) {
                if (check != i && g_browsers[check].wm_handle == reuse_handle) {
                    g_browsers[check].wm_handle = -1;
                }
            }
            b->wm_handle = reuse_handle;
            if (b->wm_handle < 0) return;
            b->drag_icon = -1;
            WM_SetClickHandler(b->wm_handle, k_click_shims[i]);
            WM_SetMouseMoveHandler(b->wm_handle, browser_mouse_move);
            WM_SetMouseReleaseHandler(b->wm_handle, browser_mouse_release);
            WM_RaiseWindow(b->wm_handle);
            WM_Redraw();
            return;
        }
    }

    /* Allocate new browser slot — recycle a closed slot first */
    int idx = -1;
    for (int i = 0; i < g_n_browsers && i < MAX_BROWSERS; i++) {
        if (g_browsers[i].volume[0] == '\0' ||
            !WM_IsWindowActive(g_browsers[i].wm_handle)) {
            idx = i;
            break;
        }
    }
    int did_increment = 0;
    if (idx < 0) {
        if (g_n_browsers >= MAX_BROWSERS) {
            FB_LOG_SCREEN("ERROR: MAX_BROWSERS");
            return;
        }
        idx = g_n_browsers++;
        did_increment = 1;
    }
    FB_LOG("[FB] Allocated slot "); FB_LOG_DEC(idx); FB_LOG("\n");
    char dbga[64];
    str_cp(dbga, "ALLOC slot ", 64);
    int da = str_len(dbga);
    dbga[da++] = '0' + idx;
    str_cp(&dbga[da], " vol=", 64-da);
    da = str_len(dbga);
    const char *vp2 = volume;
    while (*vp2 && da < 50) dbga[da++] = *vp2++;
    str_cp(&dbga[da], " n=", 64-da);
    da = str_len(dbga);
    dbga[da++] = '0' + g_n_browsers;
    dbga[da] = '\0';
    FB_LOG_SCREEN(dbga);

    Browser *b = &g_browsers[idx];
    b->wm_handle = -1;  /* Initialize to invalid - CRITICAL FIX */
    b->drag_icon = -1;
    str_cp(b->volume, volume, 32);
    char dbgc[64];
    str_cp(dbgc, "AFTER slot ", 64);
    int dc = str_len(dbgc);
    dbgc[dc++] = '0' + idx;
    dbgc[dc++] = '=';
    const char *bv3 = b->volume;
    while (*bv3 && dc < 40) dbgc[dc++] = *bv3++;
    dbgc[dc] = '\0';
    FB_LOG_SCREEN(dbgc);
    b->scroll           = 0;
    b->last_click_icon  = -1;
    b->last_click_tick  = 0;
    b->win_x = b->win_y = b->win_w = b->win_h = 0;

    /* Load entries into this browser's private buffer */
    b->entries = load_entries_for_browser(volume, &b->entry_buffer);

    /* Stagger windows so each new browser is clearly visible.
     * Clamp to desktop bounds: below menubar (20), above statusbar (18).
     * Window size is 320x240. */
    int wx = 80 + idx * 120;
    int wy = 60 + idx * 100;
    int max_x = (int)g_fb.width - 320;
    int max_y = (int)g_fb.height - 20 - 18 - 240;  /* menubar + statusbar + window */
    if (wx > max_x) wx = max_x;
    if (wx < 0) wx = 0;
    if (wy > max_y) wy = max_y;
    if (wy < 20) wy = 20;  /* MENUBAR_H */

    int new_handle = WM_AddWindow(wx, wy, 320, 240, volume,
                                  k_draw_shims[idx], browser_key);
    /* CRITICAL FIX: Check if another browser slot already has this handle.
     * Invalidate stale handle to prevent duplicates. */
    for (int check = 0; check < MAX_BROWSERS; check++) {
        if (check != idx && g_browsers[check].wm_handle == new_handle) {
            g_browsers[check].wm_handle = -1;
        }
    }
    b->wm_handle = new_handle;
    if (b->wm_handle < 0) {
        if (did_increment)
            g_n_browsers--;  /* rollback only if we incremented */
        return;
    }
    WM_SetClickHandler(b->wm_handle, k_click_shims[idx]);
    WM_SetMouseMoveHandler(b->wm_handle, browser_mouse_move);
    WM_SetMouseReleaseHandler(b->wm_handle, browser_mouse_release);
    WM_RaiseWindow(b->wm_handle);

    WM_Redraw();
}
