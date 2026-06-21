/* desktop.c — UAOS Workbench 3.x-style Desktop Renderer
 *
 * Draws a complete Workbench-inspired graphical desktop on the linear
 * framebuffer:
 *   - Menu bar (top, 20px high) with Workbench menus and clock
 *   - Desktop backdrop (solid Amiga grey)
 *   - Disk icons (VFS-mounted volumes, discovered dynamically)
 *   - Status bar (bottom, 18px)
 */

#include "desktop.h"
#include "framebuffer.h"
#include "filebrowser.h"
#include "about_win.h"
#include "shell_win.h"
#include "icon_render.h"
#include "../irq/rtc.h"
#include "../net/ntp.h"
#include "../net/timezone.h"
#include "clock_win.h"
#include "../dos/vfs.h"
#include "../dos/icon_loader.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Debug output */
#define DT_DEBUG 1
#if DT_DEBUG
    #define DT_LOG(msg) do { extern void kprint(const char *); kprint(msg); } while(0)
    #define DT_LOG_DEC(v) do { extern void kprintdec(uint32_t); kprintdec((uint32_t)(v)); } while(0)
#else
    #define DT_LOG(msg) do {} while(0)
    #define DT_LOG_DEC(v) do {} while(0)
#endif

static void scpy(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* =========================================================================
 * Layout constants
 * ========================================================================= */

#define MENUBAR_H      20
#define STATUSBAR_H    18
#define ICON_W         48
#define ICON_H         56   /* 40px bitmap + 16px label */
#define ICON_LABEL_H   16

/* =========================================================================
 * Amiga-style 3-D bevel helpers
 * (light edge top-left, dark edge bottom-right)
 * ========================================================================= */

static void draw_bevel_box(int x, int y, int w, int h, int raised)
{
    uint32_t light = raised ? WB_WHITE    : WB_DARK_GREY;
    uint32_t dark  = raised ? WB_DARK_GREY : WB_WHITE;

    FB_DrawHLine(x,         y,         w, light);          /* top            */
    FB_DrawVLine(x,         y,         h, light);          /* left           */
    FB_DrawHLine(x,         y + h - 1, w, dark);           /* bottom         */
    FB_DrawVLine(x + w - 1, y,         h, dark);           /* right          */
}

/* =========================================================================
 * Menu bar
 * ========================================================================= */

/* Fill buf[6] with the current local time as "HH:MM\0".
 * Uses the NTP epoch + timezone when synced, falls back to CMOS UTC. */
static void current_time_str(char *buf)
{
    uint8_t h, m, s;
    uint32_t epoch = ntp_get_epoch();
    if (epoch) {
        const TzInfo *tz = tz_get_current();
        int32_t  off     = tz_offset_min(tz, epoch);
        uint32_t local   = (uint32_t)((int64_t)epoch + (int64_t)off * 60);
        uint16_t yr; uint8_t mo, dy;
        ntp_unix_to_datetime(local, &yr, &mo, &dy, &h, &m, &s);
    } else {
        RtcTime t = RTC_ReadTime();
        h = t.hour; m = t.min;
    }
    buf[0] = (char)('0' + h / 10); buf[1] = (char)('0' + h % 10);
    buf[2] = ':';
    buf[3] = (char)('0' + m / 10); buf[4] = (char)('0' + m % 10);
    buf[5] = '\0';
}

/* Forward declarations */
extern void WM_Redraw(void);

/* Menu item action stubs — to be filled with real behaviour later. */

/* Workbench menu actions */
static void menu_action_backdrop(void)
{
    DT_LOG("[MENU] Backdrop selected\n");
}

static void menu_action_execute_command(void)
{
    DT_LOG("[MENU] Execute Command selected\n");
    ShellWin_Open();
}

static void menu_action_redraw_all(void)
{
    DT_LOG("[MENU] Redraw All selected\n");
    WM_Redraw();
}

static void menu_action_update_all(void)
{
    DT_LOG("[MENU] Update All selected\n");
}

static void menu_action_last_message(void)
{
    DT_LOG("[MENU] Last Message selected\n");
}

static void menu_action_about(void)
{
    DT_LOG("[MENU] About selected\n");
    AboutWin_Open();
}

static void menu_action_quit(void)
{
    DT_LOG("[MENU] Quit selected\n");
}

/* Window menu actions */
static void menu_action_new_drawer(void)
{
    DT_LOG("[MENU] New Drawer selected\n");
}

static void menu_action_open_drawer(void)
{
    DT_LOG("[MENU] Open Drawer selected\n");
}

static void menu_action_close(void)
{
    DT_LOG("[MENU] Close selected\n");
}

static void menu_action_update(void)
{
    DT_LOG("[MENU] Update selected\n");
}

static void menu_action_select_contents(void)
{
    DT_LOG("[MENU] Select Contents selected\n");
}

static void menu_action_clean_up(void)
{
    DT_LOG("[MENU] Clean Up selected\n");
}

static void menu_action_snapshot(void)
{
    DT_LOG("[MENU] Snapshot selected\n");
}

static void menu_action_show(void)
{
    DT_LOG("[MENU] Show selected\n");
}

static void menu_action_view_by(void)
{
    DT_LOG("[MENU] View By selected\n");
}

/* Icons menu actions */
static void menu_action_icon_copy(void)
{
    DT_LOG("[MENU] Copy selected\n");
}

static void menu_action_icon_rename(void)
{
    DT_LOG("[MENU] Rename selected\n");
}

static void menu_action_icon_information(void)
{
    DT_LOG("[MENU] Information selected\n");
}

static void menu_action_icon_snapshot(void)
{
    DT_LOG("[MENU] Snapshot selected\n");
}

static void menu_action_icon_unsnapshot(void)
{
    DT_LOG("[MENU] Unsnapshot selected\n");
}

static void menu_action_icon_leave_out(void)
{
    DT_LOG("[MENU] Leave Out selected\n");
}

static void menu_action_icon_put_away(void)
{
    DT_LOG("[MENU] Put Away selected\n");
}

static void menu_action_icon_delete(void)
{
    DT_LOG("[MENU] Delete selected\n");
}

static void menu_action_icon_format(void)
{
    DT_LOG("[MENU] Format selected\n");
}

static void menu_action_icon_empty_trash(void)
{
    DT_LOG("[MENU] Empty Trash selected\n");
}

/* Tools menu actions */
static void menu_action_reset_wb(void)
{
    DT_LOG("[MENU] Reset WB selected\n");
}

typedef struct {
    const char *label;
    void (*action)(void);
    int is_divider;
} MenuItem;

#define MENU_ITEM(lbl, act) { lbl, act, 0 }
#define MENU_DIVIDER        { NULL, NULL, 1 }
#define MENU_END            { NULL, NULL, 0 }

/* Menu table: index 0 = Workbench, index 1 = Window, index 2 = Icons,
 * index 3 = Tools, index 4 = Shell, index 5 = UAOS */
static const MenuItem * const g_menus[] = {
    (const MenuItem[]) {
        MENU_ITEM("Backdrop",        menu_action_backdrop        ),
        MENU_ITEM("Execute Command", menu_action_execute_command ),
        MENU_ITEM("Redraw All",      menu_action_redraw_all      ),
        MENU_ITEM("Update All",      menu_action_update_all      ),
        MENU_ITEM("Last Message",    menu_action_last_message    ),
        MENU_ITEM("About",           menu_action_about           ),
        MENU_ITEM("Quit",            menu_action_quit            ),
        MENU_END
    },
    (const MenuItem[]) {
        MENU_ITEM("New Drawer",      menu_action_new_drawer      ),
        MENU_ITEM("Open Drawer",     menu_action_open_drawer     ),
        MENU_ITEM("Close",           menu_action_close           ),
        MENU_ITEM("Update",          menu_action_update          ),
        MENU_ITEM("Select Contents", menu_action_select_contents ),
        MENU_ITEM("Clean Up",        menu_action_clean_up        ),
        MENU_ITEM("Snapshot",        menu_action_snapshot        ),
        MENU_ITEM("Show",            menu_action_show            ),
        MENU_ITEM("View By",         menu_action_view_by         ),
        MENU_END
    },
    (const MenuItem[]) {
        MENU_ITEM("Copy",            menu_action_icon_copy         ),
        MENU_ITEM("Rename",          menu_action_icon_rename       ),
        MENU_ITEM("Information",     menu_action_icon_information  ),
        MENU_ITEM("Snapshot",        menu_action_icon_snapshot     ),
        MENU_ITEM("Unsnapshot",      menu_action_icon_unsnapshot   ),
        MENU_ITEM("Leave Out",       menu_action_icon_leave_out    ),
        MENU_ITEM("Put Away",        menu_action_icon_put_away     ),
        MENU_DIVIDER,
        MENU_ITEM("Delete",          menu_action_icon_delete       ),
        MENU_ITEM("Format",          menu_action_icon_format       ),
        MENU_ITEM("Empty Trash",     menu_action_icon_empty_trash  ),
        MENU_END
    },
    (const MenuItem[]) {
        MENU_ITEM("Reset WB",        menu_action_reset_wb          ),
        MENU_END
    }
};

#define NUM_MENUS (sizeof(g_menus) / sizeof(g_menus[0]))

/* Open menu state */
static int      g_menu_index = -1;  /* -1 = none, 0 = Workbench, 1 = Window, 2 = Icons, 3 = Tools, 4 = Shell, 5 = UAOS */
static int      g_menu_hover = -1;  /* item index under pointer, -1 = none */
static int      g_menu_x     = 0;   /* dropdown screen x */
static int      g_menu_y     = 0;   /* dropdown screen y */
static int      g_menu_w     = 0;   /* dropdown width */
static int      g_menu_h     = 0;   /* dropdown height */

static int menu_item_count(const MenuItem *items)
{
    int n = 0;
    while (items[n].label || items[n].is_divider) n++;
    return n;
}

/* Compute the screen width of the longest menu label. */
static int menu_max_label_width(const MenuItem *items)
{
    int max = 0;
    for (int i = 0; items[i].label || items[i].is_divider; i++) {
        if (items[i].is_divider) continue;
        int len = 0;
        for (const char *p = items[i].label; *p; p++) len++;
        if (len * 8 > max) max = len * 8;
    }
    return max;
}

/* Compute the x coordinate of the left edge of the Nth menu title. */
static int menu_title_x(int index)
{
    const char *menus[] = { "Workbench", "Window", "Icons", "Tools", "Shell", "UAOS", NULL };
    int x = 8;
    for (int i = 0; menus[i] && i < index; i++) {
        int len = 0;
        for (const char *p = menus[i]; *p; p++) len++;
        x += len * 8 + 16;
    }
    return x;
}

static void draw_menu_dropdown(int W)
{
    if (g_menu_index < 0 || g_menu_index >= (int)NUM_MENUS) return;

    const MenuItem *items = g_menus[g_menu_index];
    int n = menu_item_count(items);
    int item_h = 16;
    int pad_x  = 8;
    int pad_y  = 2;
    int label_w = menu_max_label_width(items);
    int w = label_w + pad_x * 2;
    int h = n * item_h + pad_y * 2;

    /* Anchor to the active menu title. */
    g_menu_x = menu_title_x(g_menu_index);
    g_menu_y = MENUBAR_H;
    g_menu_w = w;
    g_menu_h = h;

    /* Clip against right edge */
    if (g_menu_x + w > W) g_menu_x = W - w;

    /* Shadow */
    FB_FillRect(g_menu_x + 4, g_menu_y + 4, w, h, WB_DARK_GREY);

    /* Menu body */
    FB_FillRect(g_menu_x, g_menu_y, w, h, WB_GREY);
    draw_bevel_box(g_menu_x, g_menu_y, w, h, 1);

    /* Items */
    for (int i = 0; i < n; i++) {
        int iy = g_menu_y + pad_y + i * item_h;
        if (items[i].is_divider) {
            FB_FillRect(g_menu_x + 2, iy, w - 4, item_h, WB_GREY);
            FB_DrawHLine(g_menu_x + pad_x, iy + item_h / 2,
                         w - pad_x * 2, WB_DARK_GREY);
            continue;
        }
        uint32_t bg = (i == g_menu_hover) ? WB_BLUE : WB_GREY;
        uint32_t fg = (i == g_menu_hover) ? WB_WHITE : WB_BLACK;
        FB_FillRect(g_menu_x + 2, iy, w - 4, item_h, bg);
        FB_PutStr(g_menu_x + pad_x, iy + (item_h - 16) / 2, items[i].label, fg, bg);
    }
}

static void draw_menubar(int W)
{
    /* Solid dark-blue background */
    FB_FillRect(0, 0, W, MENUBAR_H, WB_BLUE);

    /* Bevel bottom edge */
    FB_DrawHLine(0, MENUBAR_H - 1, W, WB_DARK_GREY);
    FB_DrawHLine(0, MENUBAR_H - 2, W, WB_LIGHT_BLUE);

    /* Menu titles */
    const char *menus[] = { "Workbench", "Window", "Icons", "Tools", "Shell", "UAOS", NULL };
    int mx = 8;
    for (int i = 0; menus[i]; i++) {
        /* Highlight the active menu title. */
        uint32_t bg = (g_menu_index == i) ? WB_LIGHT_BLUE : WB_BLUE;
        FB_PutStr(mx, 2, menus[i], WB_WHITE, bg);
        int len = 0;
        for (const char *p = menus[i]; *p; p++) len++;
        mx += len * 8 + 16;
    }

    /* Clock — show current local time (not hardcoded 00:00:00) */
    char buf[9];
    current_time_str(buf);
    FB_PutStr(W - 50, 2, buf, WB_CREAM, WB_BLUE);
}

/* =========================================================================
 * Disk icon
 * ========================================================================= */

static void draw_disk_icon(int x, int y, const char *label, uint32_t colour, int is_selected)
{
    int bx = x;
    int by = y;
    int bw = ICON_W;
    int bh = ICON_H - ICON_LABEL_H;   /* 40 */

    /* Selected state: inverse/video colours. */
    uint32_t body_col = is_selected ? (colour ^ 0x00FFFFFF) : colour;
    uint32_t slot_col = is_selected ? (WB_DARK_GREY ^ 0x00FFFFFF) : WB_DARK_GREY;
    uint32_t slot_line = is_selected ? (WB_BLACK ^ 0x00FFFFFF) : WB_BLACK;
    uint32_t label_bg = is_selected ? (WB_BLUE ^ 0x00FFFFFF) : WB_BLUE;
    uint32_t label_fg = is_selected ? (WB_WHITE ^ 0x00FFFFFF) : WB_WHITE;

    /* Icon body */
    FB_FillRect(bx, by, bw, bh, body_col);
    draw_bevel_box(bx, by, bw, bh, !is_selected);  /* swap raised/bevel in inverse */

    /* Drive slot detail */
    FB_FillRect(bx + 6, by + bh - 10, bw - 12, 5, slot_col);
    FB_DrawHLine(bx + 7, by + bh - 9, bw - 14, slot_line);

    /* Label background */
    FB_FillRect(bx, by + bh, bw, ICON_LABEL_H, label_bg);
    FB_PutStrCentred(bx, by + bh, bw, ICON_LABEL_H, label, label_fg, label_bg);
}

/* =========================================================================
 * Status bar
 * ========================================================================= */

static void draw_statusbar(int W, int H)
{
    int y = H - STATUSBAR_H;
    FB_FillRect(0, y, W, STATUSBAR_H, WB_DARK_GREY);
    FB_DrawHLine(0, y, W, WB_WHITE);

    int total = 0, running = 0, waiting = 0;
    extern void Task_GetCounts(int *, int *, int *);
    Task_GetCounts(&total, &running, &waiting);

    char buf[80];
    /* Simple sprintf replacement */
    {
        const char *prefix = "Tasks: ";
        const char *sep1   = " run / ";
        const char *sep2   = " wait / ";
        const char *suffix = " total";
        int pi = 0, bi = 0;
        while (prefix[pi] && bi < 79) buf[bi++] = prefix[pi++];
        /* running */
        if (running >= 10) buf[bi++] = (char)('0' + running / 10);
        if (running > 0 || bi == 0) buf[bi++] = (char)('0' + running % 10);
        pi = 0; while (sep1[pi] && bi < 79) buf[bi++] = sep1[pi++];
        /* waiting */
        if (waiting >= 10) buf[bi++] = (char)('0' + waiting / 10);
        if (waiting > 0 || bi == 0) buf[bi++] = (char)('0' + waiting % 10);
        pi = 0; while (sep2[pi] && bi < 79) buf[bi++] = sep2[pi++];
        /* total */
        if (total >= 10) buf[bi++] = (char)('0' + total / 10);
        buf[bi++] = (char)('0' + total % 10);
        pi = 0; while (suffix[pi] && bi < 79) buf[bi++] = suffix[pi++];
        buf[bi] = '\0';
    }
    FB_PutStr(8, y + 1, buf, WB_CREAM, WB_DARK_GREY);
}

/* =========================================================================
 * Backdrop (solid Amiga grey R:170,G:170,B:170)
 * ========================================================================= */

static void draw_backdrop(int W, int H)
{
    /* Clear the full screen first so no stale window chrome survives
     * in the menubar / statusbar bands after a resize or move */
    FB_FillRect(0, 0, W, H, WB_GREY);
}

/* =========================================================================
 * Icon state for desktop
 * ========================================================================= */

#define DBLCLICK_TICKS  2   /* max seconds between two clicks for double-click */
#define MAX_ICONS 10

typedef struct {
    int      x, y;         /* icon top-left on desktop */
    const char *volume;    /* FileBrowser_Open argument */
    const char *label;     /* Icon label text */
    int      is_ndos;      /* 1 = unformatted (NDOS) */
    int      is_selected;  /* 1 = icon currently selected/highlighted */
    uint32_t last_tick;    /* tick of last click */
    int      click_count;  /* clicks within window */
    ParsedIcon parsed;    /* loaded .info icon (zeroed if none) */
    int      has_parsed;    /* 1 if parsed icon is valid */
} IconState;

/* Desktop icon drag state */
static int      g_icon_drag_idx   = -1;
static int      g_icon_drag_off_x = 0;
static int      g_icon_drag_off_y = 0;
static int      g_icon_drag_moved = 0;
static int      g_icon_drag_orig_x = 0;
static int      g_icon_drag_orig_y = 0;

/* Desktop background double-click state */
static int       g_desktop_pressed = 0;
static uint32_t  g_desktop_last_tick = 0;
static int       g_desktop_click_count = 0;

/* Clock double-click state */
static uint32_t  g_clock_last_tick = 0;
static int       g_clock_click_count = 0;

/* Build the desktop icon list from real mounted volumes (VFS).
 * click_count / last_tick persist across calls by matching on volume name. */
static IconState *get_icons(int *count)
{
    static IconState icons[MAX_ICONS];
    static char vol_labels[MAX_ICONS][32];
    static int initialised = 0;

    if (!initialised) {
        for (int i = 0; i < MAX_ICONS; i++) {
            icons[i].volume = NULL;
            icons[i].label  = NULL;
            icons[i].is_ndos = 0;
            icons[i].is_selected = 0;
            icons[i].last_tick = 0;
            icons[i].click_count = 0;
        }
        initialised = 1;
    }

    /* Snapshot old click state so we can restore it after rebuilding.
     * Must be static — ParsedIcon is huge (~32 KB) and 10 of them on the
     * kernel stack would overflow it. */
    static IconState old_icons[MAX_ICONS];
    for (int i = 0; i < MAX_ICONS; i++) old_icons[i] = icons[i];

    int W  = (int)g_fb.width;
    int ix = W - ICON_W - 16;
    int iy = MENUBAR_H + 16;

    int n = 0;

    /* ── VFS-mounted volumes (RAM:, Workbench:, etc.) ── */
    int mount_count = VFS_GetMountCount();
    for (int mi = 0; mi < mount_count && n < MAX_ICONS; mi++) {
        char mname[32];
        if (!VFS_GetMountName(mi, mname, 32)) continue;

        /* Build the volume string (same format that will be stored in icons[n].volume) */
        const char *vol_str;
        if (str_eq(mname, "RAM")) {
            vol_str = "RAM:";
        } else {
            int li = 0;
            while (mname[li] && li < 30) {
                vol_labels[n][li] = mname[li];
                li++;
            }
            if (li < 31) vol_labels[n][li++] = ':';
            vol_labels[n][li] = '\0';
            vol_str = vol_labels[n];
        }

        /* Find previous icon with the same volume name to preserve state */
        uint32_t old_tick = 0;
        int old_clicks = 0;
        int old_selected = 0;
        int old_x = ix;
        int old_y = iy + n * (ICON_H + 8);
        for (int j = 0; j < MAX_ICONS; j++) {
            if (old_icons[j].volume && str_eq(old_icons[j].volume, vol_str)) {
                old_tick = old_icons[j].last_tick;
                old_clicks = old_icons[j].click_count;
                old_selected = old_icons[j].is_selected;
                old_x = old_icons[j].x;
                old_y = old_icons[j].y;
                break;
            }
        }

        /* Store icon data */
        icons[n].volume = vol_str;
        icons[n].label  = str_eq(mname, "RAM") ? "RAM Disk" : vol_str;
        icons[n].x = old_x;
        icons[n].y = old_y;
        icons[n].is_ndos = 0;
        icons[n].last_tick   = old_tick;
        icons[n].click_count = old_clicks;
        icons[n].is_selected = old_selected;
        icons[n].has_parsed  = 0;
        memset(&icons[n].parsed, 0, sizeof(ParsedIcon));

        /* Try to load a .info icon for this volume */
        if (Icon_Load(vol_str, &icons[n].parsed)) {
            icons[n].has_parsed = 1;
            /* Use .info label if present */
            if (icons[n].parsed.label[0]) {
                icons[n].label = icons[n].parsed.label;
            }
        }

        DT_LOG("[DT] VFS icon "); DT_LOG_DEC(n); DT_LOG(" mname='"); DT_LOG(mname); DT_LOG("' vol_str='"); DT_LOG(vol_str); DT_LOG("'\n");
        n++;
    }

    /* Clear any leftover slots */
    for (int i = n; i < MAX_ICONS; i++) {
        icons[i].volume = NULL;
        icons[i].label  = NULL;
    }

    *count = n;
    return icons;
}

/* Draw an IconState using .info image when available, else procedural fallback. */
static void draw_icon_state(const IconState *ic)
{
    if (ic->has_parsed && ic->parsed.image.width > 0) {
        int img_h = ic->parsed.image.height;
        int img_w = ic->parsed.image.width;
        int ix = ic->x + (ICON_W - img_w) / 2;
        int iy = ic->y + (ICON_H - ICON_LABEL_H - img_h) / 2;
        if (iy < ic->y) iy = ic->y;
        if (ic->is_selected) {
            Icon_DrawSelected(&ic->parsed, ix, iy);
        } else {
            Icon_Draw(&ic->parsed, ix, iy);
        }
        Icon_DrawLabel(&ic->parsed, ic->x, ic->y + ICON_H - ICON_LABEL_H, ICON_W);
    } else {
        uint32_t colour = ic->is_ndos ? WB_DARK_GREY : WB_ORANGE;
        draw_disk_icon(ic->x, ic->y, ic->label, colour, ic->is_selected);
    }
}

/* =========================================================================
 * Public entry
 * ========================================================================= */

/* Repaint a rectangular region of the desktop backdrop (stipple pattern).
 * Writes directly to the framebuffer row buffer for speed — avoids per-pixel
 * function call overhead so drag/resize does not cause full-screen flicker. */
void Desktop_RedrawRect(int rx, int ry, int rw, int rh)
{
    if (!g_fb.valid) return;
    int W = (int)g_fb.width;
    int H = (int)g_fb.height;
    int top    = MENUBAR_H;
    int bottom = H - STATUSBAR_H;

    /* Clip to backdrop area */
    int x0 = rx < 0 ? 0 : rx;
    int y0 = ry < top ? top : ry;
    int x1 = rx + rw > W ? W : rx + rw;
    int y1 = ry + rh > bottom ? bottom : ry + rh;
    if (x1 <= x0 || y1 <= y0) return;

    /* Base grey fill */
    FB_FillRect(x0, y0, x1 - x0, y1 - y0, WB_GREY);

    /* Repaint desktop icons — all icons come from get_icons (VFS + partitions) */
    {
        int n;
        IconState *icons = get_icons(&n);
        for (int i = 0; i < n; i++) {
            draw_icon_state(&icons[i]);
        }
    }

    /* Always repaint bars — a window may have overlapped them */
    draw_menubar(W);
    draw_statusbar(W, H);
}

void Desktop_Draw(void)
{
    if (!g_fb.valid) return;

    int W = (int)g_fb.width;
    int H = (int)g_fb.height;

    draw_backdrop(W, H);
    draw_menubar(W);
    draw_statusbar(W, H);

    /* Disk icons — all come from get_icons (VFS-mounted volumes + partitions) */
    {
        int n;
        IconState *icons = get_icons(&n);
        for (int i = 0; i < n; i++) {
            draw_icon_state(&icons[i]);
        }
    }

    /* Workbench dropdown menu, drawn on top of the desktop backdrop. */
    draw_menu_dropdown(W);
}

/* Workbench load control - prevents desktop from showing before LoadWB */
static int g_workbench_loaded = 0;

void Desktop_MarkWorkbenchLoaded(void)
{
    g_workbench_loaded = 1;
}

int Desktop_IsWorkbenchLoaded(void)
{
    return g_workbench_loaded;
}

/* =========================================================================
 * Tick counter — incremented by Desktop_UpdateClock (once per second)
 * Used for double-click timing: two clicks within 2 ticks = double-click
 * ========================================================================= */

static volatile uint32_t g_tick = 0;

/* Hit-test the menubar and return which menu index was clicked (-1 = none).
 * Replicates the layout logic from draw_menubar. */
static int menubar_hit(int mx, int my)
{
    if (my < 0 || my >= MENUBAR_H) return -1;
    const char *menus[] = { "Workbench", "Window", "Icons", "Tools", "Shell", "UAOS", NULL };
    int x = 8;
    for (int i = 0; menus[i]; i++) {
        int len = 0;
        for (const char *p = menus[i]; *p; p++) len++;
        int x1 = x + len * 8 + 8;  /* right edge of hit zone */
        if (mx >= x - 4 && mx < x1)
            return i;
        x += len * 8 + 16;
    }
    return -1;
}

/* Return the item index under (mx,my) when the Workbench menu is open,
 * or -1 if the point is outside the dropdown. */
static int dropdown_hit(int mx, int my)
{
    if (g_menu_index < 0 || g_menu_index >= (int)NUM_MENUS) return -1;
    if (mx < g_menu_x || mx >= g_menu_x + g_menu_w ||
        my < g_menu_y || my >= g_menu_y + g_menu_h)
        return -1;

    const MenuItem *items = g_menus[g_menu_index];
    int item_h = 16;
    int pad_y  = 2;
    int ry = my - g_menu_y - pad_y;
    if (ry < 0) return -1;
    int idx = ry / item_h;
    if (idx < 0 || idx >= menu_item_count(items)) return -1;
    if (items[idx].is_divider) return -1;
    return idx;
}

/* Update menu hover state and request a redraw if it changed.
 * Also switches to a different menu title if the cursor moves over it while
 * a menu is already open. */
static void menu_update_hover(int mx, int my)
{
    if (g_menu_index < 0) return;

    /* Switch menus if the cursor moves over another menu title. */
    if (my >= 0 && my < MENUBAR_H) {
        int menu = menubar_hit(mx, my);
        if (menu >= 0 && menu < (int)NUM_MENUS && menu != g_menu_index) {
            g_menu_index = menu;
            g_menu_hover = -1;
            WM_Redraw();
            return;
        }
    }

    int new_hover = dropdown_hit(mx, my);
    if (new_hover != g_menu_hover) {
        g_menu_hover = new_hover;
        WM_Redraw();
    }
}

int Desktop_MouseEvent(int mx, int my, int left_pressed, int right_pressed)
{
    if (!left_pressed && !right_pressed) return 0;

    /* ── Right-click on menubar: open the selected menu ───── */
    if (right_pressed && my >= 0 && my < MENUBAR_H) {
        int menu = menubar_hit(mx, my);
        if (menu >= 0 && menu < (int)NUM_MENUS) {
            g_menu_index = menu;
            g_menu_hover = -1;
            menu_update_hover(mx, my);
            WM_Redraw();
            return 1;
        }
        /* Right-click on empty menu bar area: close any open menu. */
        return 1;
    }

    /* Left-click while a menu is open: close the menu without triggering an
     * action.  This lets the user dismiss a menu with the left button. */
    if (left_pressed && g_menu_index >= 0) {
        g_menu_index = -1;
        g_menu_hover = -1;
        WM_Redraw();
        return 1;
    }

    /* ── Clock area double-click (top-right, 80px wide) ─── */
    int W_scr = (int)g_fb.width;
    if (left_pressed && my >= 0 && my < MENUBAR_H && mx >= W_scr - 50) {
        uint32_t now = g_tick;
        if (now - g_clock_last_tick <= DBLCLICK_TICKS)
            g_clock_click_count++;
        else
            g_clock_click_count = 1;
        g_clock_last_tick = now;
        if (g_clock_click_count >= 2) {
            g_clock_click_count = 0;
            ClockWin_Open();
        }
        return 1;
    }

    /* ── Left-click on menubar ──────────────────────────── */
    int menu = menubar_hit(mx, my);
    if (left_pressed && menu >= 0) {
        if (menu == 4) {
            ShellWin_Open();   /* Shell menu */
        } else if (menu == 5) {
            AboutWin_Open();   /* UAOS menu  */
        }
        return 1;
    }

    /* ── Desktop icon press (start potential drag) ─────── */
    int n;
    IconState *icons = get_icons(&n);

    DT_LOG("[DT] Checking "); DT_LOG_DEC(n); DT_LOG(" icons for hit\n");

    for (int i = 0; i < n; i++) {
        IconState *ic = &icons[i];
        if (mx >= ic->x && mx < ic->x + ICON_W &&
            my >= ic->y && my < ic->y + ICON_H) {

            DT_LOG("[DT] Icon "); DT_LOG_DEC(i); DT_LOG(" press, volume='");
            DT_LOG(ic->volume); DT_LOG("'\n");

            /* Select the clicked icon and deselect all others */
            int changed = 0;
            for (int j = 0; j < n; j++) {
                int want = (j == i) ? 1 : 0;
                if (icons[j].is_selected != want) {
                    icons[j].is_selected = want;
                    changed = 1;
                }
            }
            if (changed) WM_Redraw();

            g_icon_drag_idx    = i;
            g_icon_drag_off_x  = mx - ic->x;
            g_icon_drag_off_y  = my - ic->y;
            g_icon_drag_orig_x = ic->x;
            g_icon_drag_orig_y = ic->y;
            g_icon_drag_moved  = 0;
            g_desktop_pressed  = 0;
            return 1;
        }
    }
    /* Missed all icons — deselect all and mark as desktop background press */
    {
        int changed = 0;
        for (int j = 0; j < n; j++) {
            if (icons[j].is_selected) {
                icons[j].is_selected = 0;
                changed = 1;
            }
        }
        if (changed) WM_Redraw();
    }
    g_desktop_pressed = 1;
    return 0;
}

void Desktop_MouseHover(int mx, int my)
{
    menu_update_hover(mx, my);
}

void Desktop_RightButtonRelease(int mx, int my)
{
    (void)mx; (void)my;

    if (g_menu_index < 0) return;

    if (g_menu_hover >= 0) {
        const MenuItem *items = g_menus[g_menu_index];
        if (items[g_menu_hover].action)
            items[g_menu_hover].action();
    }

    g_menu_index = -1;
    g_menu_hover = -1;
    WM_Redraw();
}

void Desktop_MouseMove(int mx, int my, int btn_left)
{
    (void)btn_left;

    /* Menu hover tracking while the button is held. */
    menu_update_hover(mx, my);

    if (g_icon_drag_idx < 0) return;

    int n;
    IconState *icons = get_icons(&n);
    if (g_icon_drag_idx >= n) {
        g_icon_drag_idx = -1;
        return;
    }

    IconState *ic = &icons[g_icon_drag_idx];
    int new_x = mx - g_icon_drag_off_x;
    int new_y = my - g_icon_drag_off_y;

    /* Clamp to desktop bounds */
    int W = (int)g_fb.width;
    int H = (int)g_fb.height;
    int top = MENUBAR_H;
    int bottom = H - STATUSBAR_H;

    if (new_x < 0) new_x = 0;
    if (new_x > W - ICON_W) new_x = W - ICON_W;
    if (new_y < top) new_y = top;
    if (new_y > bottom - ICON_H) new_y = bottom - ICON_H;

    if (new_x != ic->x || new_y != ic->y) {
        if (new_x != g_icon_drag_orig_x || new_y != g_icon_drag_orig_y)
            g_icon_drag_moved = 1;
        ic->x = new_x;
        ic->y = new_y;
        WM_Redraw();
    }
}

void Desktop_MouseRelease(int mx, int my)
{
    (void)mx; (void)my;

    if (g_icon_drag_idx >= 0) {
        int n;
        IconState *icons = get_icons(&n);
        if (g_icon_drag_idx < n && !g_icon_drag_moved) {
            /* Treat as click — double-click logic */
            IconState *ic = &icons[g_icon_drag_idx];
            uint32_t now = g_tick;
            if (ic->click_count > 0 && (now - ic->last_tick) <= DBLCLICK_TICKS) {
                DT_LOG("[DT] Double-click icon "); DT_LOG_DEC(g_icon_drag_idx);
                DT_LOG(" vol='"); DT_LOG(ic->volume); DT_LOG("'\n");
                ic->click_count = 0;
                FileBrowser_Open(ic->volume);
            } else {
                DT_LOG("[DT] First click (release)\n");
                ic->click_count = 1;
                ic->last_tick   = now;
            }
        }
        g_icon_drag_idx = -1;
        g_desktop_pressed = 0;
        return;
    }

    /* Desktop background double-click opens a new Shell */
    if (g_desktop_pressed) {
        uint32_t now = g_tick;
        if (g_desktop_click_count > 0 && (now - g_desktop_last_tick) <= DBLCLICK_TICKS) {
            g_desktop_click_count = 0;
            ShellWin_Open();
        } else {
            g_desktop_click_count = 1;
            g_desktop_last_tick = now;
        }
        g_desktop_pressed = 0;
    }
}

int Desktop_IsDraggingIcon(void)
{
    return g_icon_drag_idx >= 0;
}

unsigned int Desktop_GetTick(void)
{
    return (unsigned int)g_tick;
}

/* Set by Desktop_UpdateClock (IRQ context) — consumed by the main loop */
static volatile int g_clock_redraw_pending = 0;

void Desktop_UpdateClock(void)
{
    if (!g_fb.valid) return;
    /* Only set a flag here — do NOT call WM_Redraw() from IRQ context.
     * WM_Redraw() takes >100 ms (full framebuffer repaint), which starves
     * the PIT IRQ while IF=0, causing g_pit_ticks to jump in a burst when
     * the IRQ returns.  That falsely advances the ntp_tick_epoch guard,
     * letting queued UIE bursts slip through and making the clock fast. */
    g_clock_redraw_pending = 1;
    g_tick++;
}

void Desktop_FlushClockRedraw(void)
{
    if (!g_clock_redraw_pending) return;
    g_clock_redraw_pending = 0;
    WM_Redraw();
}
