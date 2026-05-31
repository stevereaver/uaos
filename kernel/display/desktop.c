/* desktop.c — UAOS Workbench 3.x-style Desktop Renderer
 *
 * Draws a complete Workbench-inspired graphical desktop on the linear
 * framebuffer:
 *   - Menu bar (top, 20px high) with Workbench menus and clock
 *   - Desktop backdrop (grey stipple)
 *   - Disk icons (RAM Disk, UAOS: drive)
 *   - Status bar (bottom, 18px)
 *   - A boot/welcome window in the centre
 */

#include "desktop.h"
#include "framebuffer.h"
#include "filebrowser.h"
#include "about_win.h"
#include "shell_win.h"
#include "../irq/rtc.h"
#include "../dos/blockdev.h"
#include <stdint.h>
#include <stddef.h>

static void scpy(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
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
        /* count chars */
        int len = 0;
        for (const char *p = menus[i]; *p; p++) len++;
        mx += len * 8 + 16;
    }

    /* Clock placeholder (right-aligned) */
    FB_PutStr(W - 80, 2, "00:00:00", WB_CREAM, WB_BLUE);
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

/* Read the FAT32 volume label from a partition's boot sector.
 * Returns a pointer to a static buffer, or NULL if not a valid FAT32. */
static const char *read_fat32_vol_label(BlockDev *dev)
{
    static char label_buf[12];
    uint8_t sector[512];

    if (BlockDev_Read(dev, 0, sector, 1) != 0)
        return NULL;

    /* Check boot signature */
    if (sector[510] != 0x55 || sector[511] != 0xAA)
        return NULL;

    /* Check FAT32 signature bytes: bytes_per_sec at offset 11 should be 512 */
    uint16_t bps = sector[11] | (sector[12] << 8);
    if (bps != 512)
        return NULL;

    /* Volume label at BPB offset 71, 11 bytes, space-padded */
    int li = 0;
    for (int i = 0; i < 11; i++) {
        uint8_t c = sector[71 + i];
        if (c != ' ') label_buf[li++] = c;
    }
    label_buf[li] = '\0';

    if (li == 0)
        return NULL;

    return label_buf;
}

static IconState *get_icons(int *count)
{
    static IconState icons[MAX_ICONS];
    static char vol_labels[MAX_ICONS][16];
    static int initialised = 0;
    if (!initialised) {
        initialised = 1;
        /* Fixed icons */
        icons[0].volume      = "RAM Disk";
        icons[0].label       = "RAM Disk";
        icons[0].is_ndos     = 0;
        icons[0].last_tick   = 0;
        icons[0].click_count = 0;
        icons[1].volume      = "UAOS:";
        icons[1].label       = "UAOS:";
        icons[1].is_ndos     = 0;
        icons[1].last_tick   = 0;
        icons[1].click_count = 0;
    }
    /* Recompute positions each call in case screen size changed */
    int W  = (int)g_fb.width;
    int ix = W - ICON_W - 16;
    int iy = MENUBAR_H + 16;
    icons[0].x = ix;  icons[0].y = iy;
    icons[1].x = ix;  icons[1].y = iy + ICON_H + 8;

    int n = 2;

    /* Scan block devices for partition icons */
    BlockDev *dev = BlockDev_GetList();
    while (dev && n < MAX_ICONS) {
        if (dev->part_offset != 0) {  /* partition device */
            icons[n].x = ix;
            icons[n].y = iy + n * (ICON_H + 8);
            icons[n].volume = dev->display_name ? dev->display_name : dev->name;
            /* last_tick and click_count are NOT reset here — they must persist
             * across Desktop_Draw / get_icons calls for double-click detection. */

            /* Check formatted status (lazy, cached) */
            if (dev->formatted == 0) {
                dev->formatted = BlockDev_CheckFormatted(dev) ? 1 : -1;
            }
            icons[n].is_ndos = (dev->formatted < 0);

            if (icons[n].is_ndos) {
                static char ndos_labels[MAX_ICONS][16];
                scpy(ndos_labels[n], "NDOS:", 16);
                int nl = 0;
                while (ndos_labels[n][nl]) nl++;
                ndos_labels[n][nl++] = ' ';
                const char *dn = dev->display_name ? dev->display_name : dev->name;
                int di = 0;
                while (di < 8 && dn[di]) { ndos_labels[n][nl++] = dn[di++]; }
                ndos_labels[n][nl] = '\0';
                icons[n].label = ndos_labels[n];
            } else {
                /* Try to read the real FAT32 volume label */
                const char *vl = read_fat32_vol_label(dev);
                if (vl) {
                    scpy(vol_labels[n], vl, 16);
                    /* Append colon like Amiga volume names */
                    int vli = 0;
                    while (vol_labels[n][vli] && vli < 14) vli++;
                    if (vli < 15) {
                        vol_labels[n][vli++] = ':';
                        vol_labels[n][vli] = '\0';
                    }
                    icons[n].label = vol_labels[n];
                } else {
                    icons[n].label = dev->display_name ? dev->display_name : dev->name;
                }
            }
            n++;
        }
        dev = dev->next;
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

    /* Repaint desktop overlays that sit on the backdrop */
    int ix = W - ICON_W - 16;
    int iy = MENUBAR_H + 16;
    draw_disk_icon(ix, iy,              "RAM Disk", WB_ORANGE);
    draw_disk_icon(ix, iy + ICON_H + 8, "UAOS:",    FB_RGB(0x44, 0x88, 0xFF));

    /* Dynamic partition icons */
    {
        int n;
        IconState *icons = get_icons(&n);
        for (int i = 2; i < n; i++) {
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

    /* Disk icons — top-right corner, stacked vertically */
    int ix = W - ICON_W - 16;
    int iy = MENUBAR_H + 16;
    draw_disk_icon(ix, iy,               "RAM Disk",  WB_ORANGE);
    draw_disk_icon(ix, iy + ICON_H + 8,  "UAOS:",     FB_RGB(0x44, 0x88, 0xFF));

    /* Dynamic partition icons below fixed ones */
    {
        int n;
        (void)get_icons(&n);  /* forces layout computation */
        IconState *icons = get_icons(&n);
        for (int i = 2; i < n; i++) {
            IconState *ic = &icons[i];
            uint32_t colour = ic->is_ndos ? WB_DARK_GREY : WB_ORANGE;
            draw_disk_icon(ic->x, ic->y, ic->label, colour);
        }
    }

    /* Centre welcome window */
    draw_welcome_window(W, H);
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

    /* ── Desktop icon double-click ──────────────────────── */
    int n;
    IconState *icons = get_icons(&n);

    for (int i = 0; i < n; i++) {
        IconState *ic = &icons[i];
        if (mx >= ic->x && mx < ic->x + ICON_W &&
            my >= ic->y && my < ic->y + ICON_H) {

            uint32_t now = g_tick;
            if (ic->click_count > 0 && (now - ic->last_tick) <= DBLCLICK_TICKS) {
                ic->click_count = 0;
                FileBrowser_Open(ic->volume);
            } else {
                ic->click_count = 1;
                ic->last_tick   = now;
            }
            return 1;
        }
    }
    return 0;
}

unsigned int Desktop_GetTick(void)
{
    return (unsigned int)g_tick;
}

void Desktop_UpdateClock(void)
{
    if (!g_fb.valid) return;

    RtcTime t = RTC_ReadTime();

    /* Format as HH:MM:SS */
    char buf[9];
    buf[0] = (char)('0' + t.hour / 10);
    buf[1] = (char)('0' + t.hour % 10);
    buf[2] = ':';
    buf[3] = (char)('0' + t.min  / 10);
    buf[4] = (char)('0' + t.min  % 10);
    buf[5] = ':';
    buf[6] = (char)('0' + t.sec  / 10);
    buf[7] = (char)('0' + t.sec  % 10);
    buf[8] = '\0';

    /* Repaint just the clock rectangle in the menu bar (right-aligned, 8 chars) */
    int W = (int)g_fb.width;
    int cx = W - 80;
    FB_FillRect(cx, 0, 80, MENUBAR_H - 2, WB_BLUE);
    FB_PutStr(cx, 2, buf, WB_CREAM, WB_BLUE);

    g_tick++;
}
