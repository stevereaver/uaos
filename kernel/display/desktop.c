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
#include "../exec/intuition_lib.h"
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

/* MENUBAR_H is defined in desktop.h */
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

/* Screen title state (updated by intuition.library ShowTitle()) */
static char g_screen_title[64] = "";
static int  g_show_screen_title = 0;

/* DisplayBeep flash state */
static uint32_t g_beep_flash_color = 0;
static uint64_t g_beep_flash_until = 0;
extern volatile uint64_t g_pit_ticks;

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
static int      g_menu_index = -1;   /* -1 = none */
static int      g_menu_hover = -1;   /* item index under pointer, -1 = none */
static int      g_menu_x     = 0;    /* dropdown screen x */
static int      g_menu_y     = 0;    /* dropdown screen y */
static int      g_menu_w     = 0;    /* dropdown width */
static int      g_menu_h     = 0;    /* dropdown height */
static int      g_submenu_item = -1; /* top-level item with open submenu, -1 = none */
static int      g_submenu_hover = -1;/* submenu item under pointer, -1 = none */
static int      g_submenu_x  = 0;  /* submenu screen x */
static int      g_submenu_y  = 0;  /* submenu screen y */
static int      g_submenu_w  = 0;  /* submenu width */
static int      g_submenu_h  = 0;  /* submenu height */

/* Active guest menu strip (parsed from the focused window) */
static HostMenu  g_active_menus[HOST_MENU_MAX];
static int       g_active_menu_count = 0;
static int       g_guest_menu_active = 0;  /* 1 = use g_active_menus, post IDCMP_MENUPICK */

static int menu_item_count(const MenuItem *items)
{
    int n = 0;
    while (items[n].label || items[n].is_divider) n++;
    return n;
}

/* Refresh the active menu strip from the focused guest window.
 * Falls back to the hardcoded desktop menu when no guest strip is present. */
static void refresh_active_menus(void)
{
    uint32_t strip = Intuition_GetActiveWindowMenuStrip();
    if (strip) {
        int n = Intuition_GetHostMenuStrip(strip, g_active_menus, HOST_MENU_MAX);
        if (n > 0) {
            g_active_menu_count = n;
            g_guest_menu_active = 1;
            return;
        }
    }
    g_active_menu_count = 0;
    g_guest_menu_active = 0;
}

/* Compute the screen width of the longest menu label in a fallback MenuItem list. */
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

/* Compute the screen width of the longest menu label in an active HostMenu. */
static int host_menu_max_label_width(const HostMenu *menu)
{
    int max = 0;
    for (int i = 0; i < menu->item_count; i++) {
        if (!menu->items[i].label[0]) continue;
        int len = 0;
        for (const char *p = menu->items[i].label; *p; p++) len++;
        if (len * 8 > max) max = len * 8;
    }
    return max;
}

/* Return the title of the Nth menu, either from the active guest strip or
 * the hardcoded fallback titles. */
static const char *menu_title(int index)
{
    if (g_guest_menu_active && index >= 0 && index < g_active_menu_count)
        return g_active_menus[index].label;
    const char *fallback[] = { "Workbench", "Window", "Icons", "Tools" };
    if (index >= 0 && index < (int)(sizeof(fallback) / sizeof(fallback[0])))
        return fallback[index];
    return NULL;
}

/* Compute the x coordinate of the left edge of the Nth menu title. */
static int menu_title_x(int index)
{
    int x = 8;
    for (int i = 0; i < index; i++) {
        const char *title = menu_title(i);
        if (!title) break;
        int len = 0;
        for (const char *p = title; *p; p++) len++;
        x += len * 8 + 16;
    }
    return x;
}

static void draw_host_menu_item(const HostMenuItem *item, int x, int y, int w,
                                int item_h, int pad_x, int is_hover)
{
    uint32_t bg = is_hover ? WB_BLUE : WB_GREY;
    uint32_t fg = is_hover ? WB_WHITE : WB_BLACK;
    if (!item->enabled) {
        fg = WB_DARK_GREY;
        bg = WB_GREY;
    }

    int cx = x + pad_x;
    int cy = y + (item_h - 16) / 2;

    /* Highlight bar */
    FB_FillRect(x + 2, y, w - 4, item_h, bg);

    /* Checkmark box for CHECKIT items (column is always reserved). */
    int box = 10;
    int bx = cx;
    int by = y + (item_h - box) / 2;
    if (item->has_checkmark) {
        FB_DrawRect(bx, by, box, box, fg);
        if (item->checked) {
            FB_FillRect(bx + 2, by + 2, box - 4, box - 4, fg);
        }
    }
    cx += 14;

    /* Label */
    FB_PutStr(cx, cy, item->label, fg, bg);

    /* Right-side extras: command key or submenu arrow */
    int rx = x + w - pad_x - 8;
    if (item->has_submenu) {
        FB_PutStr(rx - 4, cy, ">", fg, bg);
    } else if (item->command_key) {
        char key[2] = { item->command_key, '\0' };
        FB_PutStr(rx, cy, key, fg, bg);
    }
}

static int host_menu_item_width(const HostMenuItem *item)
{
    int len = 0;
    for (const char *p = item->label; *p; p++) len++;
    int w = 8 * 2 + 14 + len * 8; /* left+right margins + checkmark column + label */
    if (item->has_submenu || item->command_key) w += 16;
    return w;
}

static int host_menu_dropdown_width(const HostMenu *menu)
{
    int max = 0;
    for (int i = 0; i < menu->item_count; i++) {
        int w = host_menu_item_width(&menu->items[i]);
        if (w > max) max = w;
    }
    return max;
}

static void draw_host_submenu(const HostMenu *submenu, int x, int y, int W)
{
    int item_h = 16;
    int pad_x = 8;
    int pad_y = 2;
    int n = submenu->item_count;
    if (n <= 0) return;

    int label_w = host_menu_dropdown_width(submenu);
    int w = label_w + pad_x * 2;
    int h = n * item_h + pad_y * 2;

    if (x + w > W) x = W - w;
    if (y + h > (int)g_fb.height) y = (int)g_fb.height - h;
    if (y < 0) y = 0;

    g_submenu_x = x;
    g_submenu_y = y;
    g_submenu_w = w;
    g_submenu_h = h;

    FB_FillRect(g_submenu_x + 4, g_submenu_y + 4, w, h, WB_DARK_GREY);
    FB_FillRect(g_submenu_x, g_submenu_y, w, h, WB_GREY);
    draw_bevel_box(g_submenu_x, g_submenu_y, w, h, 1);

    for (int i = 0; i < n; i++) {
        int iy = g_submenu_y + pad_y + i * item_h;
        draw_host_menu_item(&submenu->items[i], g_submenu_x, iy, w,
                            item_h, pad_x, i == g_submenu_hover);
    }
}

static void draw_menu_dropdown(int W)
{
    if (g_guest_menu_active) {
        if (g_menu_index < 0 || g_menu_index >= g_active_menu_count) return;
        const HostMenu *menu = &g_active_menus[g_menu_index];
        int n = menu->item_count;
        if (n <= 0) return;
        int item_h = 16;
        int pad_x  = 8;
        int pad_y  = 2;
        int label_w = host_menu_dropdown_width(menu);
        int w = label_w + pad_x * 2;
        int h = n * item_h + pad_y * 2;

        g_menu_x = menu_title_x(g_menu_index);
        g_menu_y = MENUBAR_H;
        g_menu_w = w;
        g_menu_h = h;

        if (g_menu_x + w > W) g_menu_x = W - w;

        FB_FillRect(g_menu_x + 4, g_menu_y + 4, w, h, WB_DARK_GREY);
        FB_FillRect(g_menu_x, g_menu_y, w, h, WB_GREY);
        draw_bevel_box(g_menu_x, g_menu_y, w, h, 1);

        for (int i = 0; i < n; i++) {
            int iy = g_menu_y + pad_y + i * item_h;
            draw_host_menu_item(&menu->items[i], g_menu_x, iy, w,
                                item_h, pad_x, i == g_menu_hover);
        }

        if (g_submenu_item >= 0 && g_submenu_item < n && menu->items[g_submenu_item].has_submenu) {
            int sx = g_menu_x + g_menu_w - 2;
            int sy = g_menu_y + pad_y + g_submenu_item * item_h;
            draw_host_submenu(menu->items[g_submenu_item].submenu, sx, sy, W);
        }
        return;
    }

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
    /* Update the active menu strip from the focused guest window. */
    refresh_active_menus();

    /* Top bevel: black outer, white inner (K+W+B+...) */
    FB_DrawHLine(0, 0, W, WB_BLACK);
    FB_DrawHLine(0, 1, W, WB_WHITE);
    /* Blue fill between the white highlight and the black bottom */
    FB_FillRect(0, 2, W, MENUBAR_H - 3, WB_BLUE);
    /* Black bottom edge */
    FB_DrawHLine(0, MENUBAR_H - 1, W, WB_BLACK);

    /* Menu titles (Topaz 8, vertically fills the 8px blue area y=2..9) */
    int mx = 8;
    for (int i = 0; ; i++) {
        const char *title = menu_title(i);
        if (!title) break;
        /* Highlight the active menu title with a black bar, white text. */
        uint32_t bg = (g_menu_index == i) ? WB_BLACK : WB_BLUE;
        FB_PutStrSmall(mx, 2, title, WB_WHITE, bg);
        int len = 0;
        for (const char *p = title; *p; p++) len++;
        mx += len * 8 + 16;
    }

    /* Screen title — drawn between menus and clock when requested */
    if (g_show_screen_title && g_screen_title[0]) {
        int title_w = 0;
        for (const char *p = g_screen_title; *p; p++) title_w += 8;
        int title_x = mx + 16;
        if (title_x + title_w > W - 58)
            title_x = W - 58 - title_w;
        if (title_x > mx && title_w > 0)
            FB_PutStrSmall(title_x, 2, g_screen_title, WB_WHITE, WB_BLUE);
    }

    /* Clock — show current local time (not hardcoded 00:00:00) */
    char buf[9];
    current_time_str(buf);
    FB_PutStrSmall(W - 50, 2, buf, WB_CREAM, WB_BLUE);
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
    uint32_t bg = WB_GREY;
    if (g_beep_flash_color && g_beep_flash_until && g_pit_ticks < g_beep_flash_until)
        bg = g_beep_flash_color;
    FB_FillRect(0, 0, W, H, bg);

    /* If the front Intuition screen has a custom SA_BitMap, render it as
     * the desktop backdrop.  Windows and the menu/status bars are drawn on
     * top by the WM. */
    UAOS_Intuition_RenderScreenBackdrop();
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

/* Desktop lasso (rubber-band) selection state.
 * Active when the user presses the left button on empty desktop backdrop
 * and drags — a dashed rectangle follows the cursor and any icon whose
 * bounding box intersects it is selected.  Classic Workbench behaviour. */
static int g_lasso_active  = 0;
static int g_lasso_start_x = 0;
static int g_lasso_start_y = 0;
static int g_lasso_cur_x   = 0;
static int g_lasso_cur_y   = 0;
static int g_lasso_moved   = 0;  /* 1 once the cursor moved during the drag */

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
 * Lasso (rubber-band) selection rectangle
 * ========================================================================= */

/* Draw a dashed horizontal line — 1px on / 1px off, black.
 * Approximates the Workbench marquee selection border. */
static void draw_dashed_hline(int x, int y, int len)
{
    for (int i = 0; i < len; i += 2)
        FB_PutPixel(x + i, y, WB_BLACK);
}

/* Draw a dashed vertical line — 1px on / 1px off, black. */
static void draw_dashed_vline(int x, int y, int len)
{
    for (int i = 0; i < len; i += 2)
        FB_PutPixel(x, y + i, WB_BLACK);
}

/* Draw the lasso rectangle if active.  Clipped to the desktop backdrop
 * area (between menu bar and status bar) so it never overdraws chrome. */
static void draw_lasso(int W, int H)
{
    if (!g_lasso_active) return;

    int top    = MENUBAR_H;
    int bottom = H - STATUSBAR_H;

    /* Normalise the rectangle regardless of drag direction */
    int x0 = g_lasso_start_x < g_lasso_cur_x ? g_lasso_start_x : g_lasso_cur_x;
    int y0 = g_lasso_start_y < g_lasso_cur_y ? g_lasso_start_y : g_lasso_cur_y;
    int x1 = g_lasso_start_x < g_lasso_cur_x ? g_lasso_cur_x   : g_lasso_start_x;
    int y1 = g_lasso_start_y < g_lasso_cur_y ? g_lasso_cur_y   : g_lasso_start_y;

    /* Clip to desktop backdrop */
    if (y0 < top)    y0 = top;
    if (y1 >= bottom) y1 = bottom - 1;
    if (x0 < 0)      x0 = 0;
    if (x1 >= W)     x1 = W - 1;
    if (x1 <= x0 || y1 <= y0) return;

    int w = x1 - x0 + 1;
    int h = y1 - y0 + 1;

    draw_dashed_hline(x0, y0, w);
    draw_dashed_hline(x0, y1, w);
    draw_dashed_vline(x0, y0, h);
    draw_dashed_vline(x1, y0, h);
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

    /* Lasso rectangle on top of icons, below bars */
    draw_lasso(W, H);

    /* Always repaint bars — a window may have overlapped them */
    draw_menubar(W);
    draw_statusbar(W, H);
}

void Desktop_Draw(void)
{
    if (!g_fb.valid) return;

    /* Use the front Intuition screen's palette for desktop chrome. */
    UAOS_Intuition_ApplyFrontScreenPalette();

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

    /* Lasso rectangle on top of icons, below menu dropdown */
    draw_lasso(W, H);

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

void Desktop_SetScreenTitle(const char *title, int show)
{
    g_show_screen_title = show;
    if (title) {
        int i = 0;
        while (i < (int)sizeof(g_screen_title) - 1 && title[i]) {
            g_screen_title[i] = title[i];
            i++;
        }
        g_screen_title[i] = '\0';
    } else {
        g_screen_title[0] = '\0';
    }
    WM_Redraw();
}

void Desktop_DisplayBeepFlash(uint32_t color)
{
    if (color) {
        g_beep_flash_color = color;
        g_beep_flash_until = g_pit_ticks + 5;  /* 50 ms at 100 Hz */
    } else {
        g_beep_flash_color = 0;
        g_beep_flash_until = 0;
    }
}

/* =========================================================================
 * Tick counter — incremented by Desktop_UpdateClock (once per second)
 * Used for double-click timing: two clicks within 2 ticks = double-click
 * ========================================================================= */

static volatile uint32_t g_tick = 0;

/* Return the number of available menus (guest strip or fallback). */
static int active_menu_count(void)
{
    return g_guest_menu_active ? g_active_menu_count : (int)NUM_MENUS;
}

/* Hit-test the menubar and return which menu index was clicked (-1 = none).
 * Replicates the layout logic from draw_menubar. */
static int menubar_hit(int mx, int my)
{
    if (my < 0 || my >= MENUBAR_H) return -1;
    int x = 8;
    for (int i = 0; i < active_menu_count(); i++) {
        const char *title = menu_title(i);
        if (!title) break;
        int len = 0;
        for (const char *p = title; *p; p++) len++;
        int x1 = x + len * 8 + 8;  /* right edge of hit zone */
        if (mx >= x - 4 && mx < x1)
            return i;
        x += len * 8 + 16;
    }
    return -1;
}

/* Return the item index under (mx,my) when a menu is open,
 * or -1 if the point is outside the dropdown. */
static int dropdown_hit(int mx, int my)
{
    if (g_menu_index < 0 || g_menu_index >= active_menu_count()) return -1;
    if (mx < g_menu_x || mx >= g_menu_x + g_menu_w ||
        my < g_menu_y || my >= g_menu_y + g_menu_h)
        return -1;

    int item_h = 16;
    int pad_y  = 2;
    int ry = my - g_menu_y - pad_y;
    if (ry < 0) return -1;
    int idx = ry / item_h;

    if (g_guest_menu_active) {
        const HostMenu *menu = &g_active_menus[g_menu_index];
        if (idx < 0 || idx >= menu->item_count) return -1;
        if (!menu->items[idx].enabled) return -1;
        return idx;
    }

    const MenuItem *items = g_menus[g_menu_index];
    if (idx < 0 || idx >= menu_item_count(items)) return -1;
    if (items[idx].is_divider) return -1;
    return idx;
}

/* Return the submenu item index under (mx,my) when a submenu is open,
 * or -1 if the point is outside the submenu. */
static int submenu_hit(int mx, int my)
{
    if (g_submenu_item < 0) return -1;
    if (mx < g_submenu_x || mx >= g_submenu_x + g_submenu_w ||
        my < g_submenu_y || my >= g_submenu_y + g_submenu_h)
        return -1;

    int item_h = 16;
    int pad_y  = 2;
    int ry = my - g_submenu_y - pad_y;
    if (ry < 0) return -1;
    int idx = ry / item_h;

    if (g_guest_menu_active) {
        const HostMenu *menu = &g_active_menus[g_menu_index];
        if (g_submenu_item < 0 || g_submenu_item >= menu->item_count) return -1;
        const HostMenu *sm = menu->items[g_submenu_item].submenu;
        if (!sm) return -1;
        if (idx < 0 || idx >= sm->item_count) return -1;
        if (!sm->items[idx].enabled) return -1;
        return idx;
    }
    return -1;
}

/* Update menu hover state and request a redraw if it changed.
 * Also switches to a different menu title if the cursor moves over it while
 * a menu is already open, and opens submenus for items that have them. */
static void menu_update_hover(int mx, int my)
{
    if (g_menu_index < 0) return;

    /* Switch menus if the cursor moves over another menu title. */
    if (my >= 0 && my < MENUBAR_H) {
        int menu = menubar_hit(mx, my);
        if (menu >= 0 && menu < active_menu_count() && menu != g_menu_index) {
            g_menu_index = menu;
            g_menu_hover = -1;
            g_submenu_item = -1;
            g_submenu_hover = -1;
            WM_Redraw();
            return;
        }
    }

    if (g_guest_menu_active) {
        int sub_hover = submenu_hit(mx, my);
        if (sub_hover >= 0) {
            if (sub_hover != g_submenu_hover) {
                g_submenu_hover = sub_hover;
                WM_Redraw();
            }
            return;
        }
    }

    int new_hover = dropdown_hit(mx, my);
    if (new_hover != g_menu_hover) {
        g_menu_hover = new_hover;
        g_submenu_hover = -1;
        if (g_guest_menu_active && g_menu_hover >= 0 &&
            g_menu_hover < g_active_menus[g_menu_index].item_count &&
            g_active_menus[g_menu_index].items[g_menu_hover].has_submenu) {
            g_submenu_item = g_menu_hover;
        } else {
            g_submenu_item = -1;
        }
        WM_Redraw();
    }
}

int Desktop_MouseEvent(int mx, int my, int left_pressed, int right_pressed)
{
    if (!left_pressed && !right_pressed) return 0;

    /* ── Right-click on menubar: open the selected menu ───── */
    if (right_pressed && my >= 0 && my < MENUBAR_H) {
        int menu = menubar_hit(mx, my);
        if (menu >= 0 && menu < active_menu_count()) {
            g_menu_index = menu;
            g_menu_hover = -1;
            g_submenu_item = -1;
            g_submenu_hover = -1;
            menu_update_hover(mx, my);
            WM_Redraw();
            return 1;
        }
        /* Right-click on empty menu bar area: close any open menu. */
        g_menu_index = -1;
        g_menu_hover = -1;
        g_submenu_item = -1;
        g_submenu_hover = -1;
        WM_Redraw();
        return 1;
    }

    /* Left-click while a menu is open: close the menu without triggering an
     * action.  This lets the user dismiss a menu with the left button. */
    if (left_pressed && g_menu_index >= 0) {
        g_menu_index = -1;
        g_menu_hover = -1;
        g_submenu_item = -1;
        g_submenu_hover = -1;
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
            g_lasso_active     = 0;
            return 1;
        }
    }
    /* Missed all icons — deselect all, start a lasso for potential drag
     * selection, and mark as desktop background press for double-click. */
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
    g_lasso_active  = 1;
    g_lasso_start_x = mx;
    g_lasso_start_y = my;
    g_lasso_cur_x   = mx;
    g_lasso_cur_y   = my;
    g_lasso_moved   = 0;
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

    if (g_guest_menu_active) {
        HostMenuItem *mi = NULL;
        uint32_t menu_number = 0;
        if (g_submenu_item >= 0 && g_submenu_hover >= 0) {
            const HostMenu *menu = &g_active_menus[g_menu_index];
            if (g_submenu_item < menu->item_count && menu->items[g_submenu_item].submenu) {
                mi = &menu->items[g_submenu_item].submenu->items[g_submenu_hover];
                menu_number = (uint32_t)((g_menu_index & 0x1F) |
                                         ((g_submenu_item & 0x3F) << 5) |
                                         ((g_submenu_hover & 0x1F) << 11));
            }
        } else if (g_menu_hover >= 0) {
            mi = &g_active_menus[g_menu_index].items[g_menu_hover];
            menu_number = (uint32_t)((g_menu_index & 0x1F) |
                                     ((g_menu_hover & 0x3F) << 5));
        }
        if (mi) {
            Intuition_UpdateMenuItemCheck(mi->guest_item, mi->toggle);
            Intuition_PostMenuPick(menu_number);
        }
    } else {
        if (g_menu_hover >= 0) {
            const MenuItem *items = g_menus[g_menu_index];
            if (items[g_menu_hover].action)
                items[g_menu_hover].action();
        }
    }

    g_menu_index = -1;
    g_menu_hover = -1;
    g_submenu_item = -1;
    g_submenu_hover = -1;
    WM_Redraw();
}

void Desktop_MouseMove(int mx, int my, int btn_left)
{
    (void)btn_left;

    /* Menu hover tracking while the button is held. */
    menu_update_hover(mx, my);

    /* Lasso (rubber-band) selection — update the drag rectangle and
     * select every icon whose bounding box intersects it. */
    if (g_lasso_active) {
        if (mx == g_lasso_cur_x && my == g_lasso_cur_y) return;
        g_lasso_cur_x = mx;
        g_lasso_cur_y = my;
        g_lasso_moved = 1;

        /* Normalise lasso rectangle (drag may go any direction) */
        int lx0 = g_lasso_start_x < g_lasso_cur_x ? g_lasso_start_x : g_lasso_cur_x;
        int ly0 = g_lasso_start_y < g_lasso_cur_y ? g_lasso_start_y : g_lasso_cur_y;
        int lx1 = g_lasso_start_x < g_lasso_cur_x ? g_lasso_cur_x   : g_lasso_start_x;
        int ly1 = g_lasso_start_y < g_lasso_cur_y ? g_lasso_cur_y   : g_lasso_start_y;

        int n;
        IconState *icons = get_icons(&n);
        int changed = 0;
        for (int i = 0; i < n; i++) {
            IconState *ic = &icons[i];
            /* AABB intersection: icon bbox vs lasso rect */
            int hit = !(ic->x + ICON_W <= lx0 || ic->x >= lx1 + 1 ||
                        ic->y + ICON_H <= ly0 || ic->y >= ly1 + 1);
            if (ic->is_selected != hit) {
                ic->is_selected = hit;
                changed = 1;
            }
        }
        /* Always redraw — the lasso rectangle itself moved even if no
         * icon selection changed. */
        WM_Redraw();
        (void)changed;
        return;
    }

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

    /* Desktop background double-click opens a new Shell.
     * A lasso drag (cursor moved) is NOT a click — suppress double-click.
     * Also ends lasso selection — the current selection is kept. */
    if (g_desktop_pressed) {
        if (!g_lasso_moved) {
            uint32_t now = g_tick;
            if (g_desktop_click_count > 0 && (now - g_desktop_last_tick) <= DBLCLICK_TICKS) {
                g_desktop_click_count = 0;
                ShellWin_Open();
            } else {
                g_desktop_click_count = 1;
                g_desktop_last_tick = now;
            }
        }
        g_desktop_pressed = 0;
    }
    if (g_lasso_active) {
        g_lasso_active = 0;
        WM_Redraw();
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
