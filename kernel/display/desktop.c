/* desktop.c — UAOS Workbench 3.x-style Desktop Renderer
 *
 * Draws a complete Workbench-inspired graphical desktop on the linear
 * framebuffer:
 *   - Menu bar (top, 20px high) with Workbench menus and clock
 *   - Desktop backdrop (grey stipple)
 *   - Disk icons (VFS-mounted volumes, discovered dynamically)
 *   - Status bar (bottom, 18px)
 *   - A boot/welcome window in the centre
 */

#include "desktop.h"
#include "framebuffer.h"
#include "filebrowser.h"
#include "about_win.h"
#include "shell_win.h"
#include "../irq/rtc.h"
#include "../net/ntp.h"
#include "../net/timezone.h"
#include "../dos/vfs.h"
#include <stdint.h>
#include <stddef.h>

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

/* Fill buf[9] with the current local time as "HH:MM:SS\0".
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
        h = t.hour; m = t.min; s = t.sec;
    }
    buf[0] = (char)('0' + h / 10); buf[1] = (char)('0' + h % 10);
    buf[2] = ':';
    buf[3] = (char)('0' + m / 10); buf[4] = (char)('0' + m % 10);
    buf[5] = ':';
    buf[6] = (char)('0' + s / 10); buf[7] = (char)('0' + s % 10);
    buf[8] = '\0';
}

static void draw_menubar(int W)
{
    /* Solid dark-blue background */
    FB_FillRect(0, 0, W, MENUBAR_H, WB_BLUE);

    /* Bevel bottom edge */
    FB_DrawHLine(0, MENUBAR_H - 1, W, WB_DARK_GREY);
    FB_DrawHLine(0, MENUBAR_H - 2, W, WB_LIGHT_BLUE);

    /* Menu titles */
    const char *menus[] = { "Workbench", "Window", "Shell", "UAOS", NULL };
    int mx = 8;
    for (int i = 0; menus[i]; i++) {
        FB_PutStr(mx, 2, menus[i], WB_WHITE, WB_BLUE);
        int len = 0;
        for (const char *p = menus[i]; *p; p++) len++;
        mx += len * 8 + 16;
    }

    /* Clock — show current local time (not hardcoded 00:00:00) */
    char buf[9];
    current_time_str(buf);
    FB_PutStr(W - 80, 2, buf, WB_CREAM, WB_BLUE);
}

/* =========================================================================
 * Disk icon
 * ========================================================================= */

static void draw_disk_icon(int x, int y, const char *label, uint32_t colour)
{
    int bx = x;
    int by = y;
    int bw = ICON_W;
    int bh = ICON_H - ICON_LABEL_H;   /* 40 */

    /* Icon body */
    FB_FillRect(bx, by, bw, bh, colour);
    draw_bevel_box(bx, by, bw, bh, 1);

    /* Drive slot detail */
    FB_FillRect(bx + 6, by + bh - 10, bw - 12, 5, WB_DARK_GREY);
    FB_DrawHLine(bx + 7, by + bh - 9, bw - 14, WB_BLACK);

    /* Label background */
    FB_FillRect(bx, by + bh, bw, ICON_LABEL_H, WB_BLUE);
    FB_PutStrCentred(bx, by + bh, bw, ICON_LABEL_H, label, WB_WHITE, WB_BLUE);
}

/* =========================================================================
 * Welcome / boot information window
 * ========================================================================= */

static void draw_welcome_window(int W, int H)
{
    int ww = 480, wh = 200;
    int wx = (W - ww) / 2;
    int wy = (H - wh) / 2;

    /* Window shadow */
    FB_FillRect(wx + 4, wy + 4, ww, wh, WB_DARK_GREY);

    /* Window body */
    FB_FillRect(wx, wy, ww, wh, WB_GREY);
    draw_bevel_box(wx, wy, ww, wh, 1);

    /* Title bar */
    FB_FillRect(wx + 1, wy + 1, ww - 2, MENUBAR_H, WB_LIGHT_BLUE);
    FB_PutStrCentred(wx + 1, wy + 1, ww - 2, MENUBAR_H,
                     "UAOS  v0.1.0-dev  Information", WB_WHITE, WB_LIGHT_BLUE);

    /* Close gadget */
    FB_FillRect(wx + 2, wy + 2, 16, 16, WB_GREY);
    draw_bevel_box(wx + 2, wy + 2, 16, 16, 1);

    /* Depth gadget */
    FB_FillRect(wx + ww - 18, wy + 2, 16, 16, WB_GREY);
    draw_bevel_box(wx + ww - 18, wy + 2, 16, 16, 1);

    /* Separator below title bar */
    FB_DrawHLine(wx + 1, wy + MENUBAR_H + 1, ww - 2, WB_DARK_GREY);

    /* Content */
    int cx = wx + 16;
    int cy = wy + MENUBAR_H + 12;
    int ls = 20;   /* line spacing */

    FB_PutStr(cx, cy,        "Ultimate Amiga OS (UAOS) kernel is running.", WB_BLACK, WB_GREY);
    cy += ls;
    FB_PutStr(cx, cy,        "Architecture : x86_64  (64-bit long mode)", WB_BLACK, WB_GREY);
    cy += ls;
    FB_PutStr(cx, cy,        "Boot protocol: Multiboot2 via GRUB2/OVMF", WB_BLACK, WB_GREY);
    cy += ls;
    FB_PutStr(cx, cy,        "M68k emulator: Musashi 3.32 (68000, run <prog> in Shell)", WB_BLACK, WB_GREY);
    cy += ls + 4;
    FB_DrawHLine(wx + 8, cy, ww - 16, WB_DARK_GREY);
    cy += 8;
    FB_PutStr(cx, cy,        "Keyboard: PS/2 not yet initialised.", WB_DARK_GREY, WB_GREY);
}

/* =========================================================================
 * Status bar
 * ========================================================================= */

static void draw_statusbar(int W, int H)
{
    int y = H - STATUSBAR_H;
    FB_FillRect(0, y, W, STATUSBAR_H, WB_DARK_GREY);
    FB_DrawHLine(0, y, W, WB_WHITE);
    FB_PutStr(8, y + 1, "UAOS kernel idle  |  No tasks running  |  RAM: 512 MB", WB_CREAM, WB_DARK_GREY);
}

/* =========================================================================
 * Backdrop stipple (alternating light/dark grey pixels — classic Amiga look)
 * ========================================================================= */

static void draw_backdrop(int W, int H)
{
    /* Clear the full screen first so no stale window chrome survives
     * in the menubar / statusbar bands after a resize or move */
    FB_FillRect(0, 0, W, H, WB_GREY);

    int top    = MENUBAR_H;
    int bottom = H - STATUSBAR_H;

    /* Overlay dark dots at every other position for stipple effect */
    for (int y = top; y < bottom; y += 2)
        for (int x = y & 2 ? 0 : 1; x < W; x += 2)
            FB_PutPixel(x, y, WB_DARK_GREY);
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
    uint32_t last_tick;    /* tick of last click */
    int      click_count;  /* clicks within window */
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

extern void WM_Redraw(void);

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
            icons[i].last_tick = 0;
            icons[i].click_count = 0;
        }
        initialised = 1;
    }

    /* Snapshot old click state so we can restore it after rebuilding */
    IconState old_icons[MAX_ICONS];
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
        int old_x = ix;
        int old_y = iy + n * (ICON_H + 8);
        for (int j = 0; j < MAX_ICONS; j++) {
            if (old_icons[j].volume && str_eq(old_icons[j].volume, vol_str)) {
                old_tick = old_icons[j].last_tick;
                old_clicks = old_icons[j].click_count;
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

    /* Overlay stipple dots — same pattern as draw_backdrop.
     * draw_backdrop: for (y = top; y < bottom; y += 2) — dots every 2 rows.
     * Start y at x0's first matching row parity, then step by 2. */
    int y_start = y0;
    if ((y_start & 1) != (top & 1)) y_start++; /* align to same even/odd as top */
    for (int y = y_start; y < y1; y += 2) {
        int phase = (y & 2) ? 0 : 1;  /* phase 0: x=0,2,4…  phase 1: x=1,3,5… */
        int x = x0;
        if ((x & 1) != phase) x++;    /* nudge to correct parity */
        for (; x < x1; x += 2)
            FB_PutPixel(x, y, WB_DARK_GREY);
    }

    /* Repaint desktop icons — all icons come from get_icons (VFS + partitions) */
    {
        int n;
        IconState *icons = get_icons(&n);
        for (int i = 0; i < n; i++) {
            IconState *ic = &icons[i];
            uint32_t colour = ic->is_ndos ? WB_DARK_GREY : WB_ORANGE;
            draw_disk_icon(ic->x, ic->y, ic->label, colour);
        }
    }

    draw_welcome_window(W, H);

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
            IconState *ic = &icons[i];
            uint32_t colour = ic->is_ndos ? WB_DARK_GREY : WB_ORANGE;
            draw_disk_icon(ic->x, ic->y, ic->label, colour);
        }
    }

    /* Centre welcome window */
    draw_welcome_window(W, H);
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
    const char *menus[] = { "Workbench", "Window", "Shell", "UAOS", NULL };
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

int Desktop_MouseEvent(int mx, int my, int btn_pressed)
{
    if (!btn_pressed) return 0;

    /* ── Menubar click ──────────────────────────────────── */
    int menu = menubar_hit(mx, my);
    if (menu >= 0) {
        if (menu == 2) ShellWin_Open();   /* Shell menu */
        if (menu == 3) AboutWin_Open();   /* UAOS menu  */
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
    /* Missed all icons — mark as desktop background press */
    g_desktop_pressed = 1;
    return 0;
}

void Desktop_MouseMove(int mx, int my, int btn_left)
{
    (void)btn_left;
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

void Desktop_UpdateClock(void)
{
    if (!g_fb.valid) return;

    /* Full repaint via the WM — this repaints the menubar (which reads the
     * current local time via current_time_str) and all open windows including
     * the Clock app.  Called once per second from the RTC IRQ handler. */
    WM_Redraw();

    g_tick++;
}
