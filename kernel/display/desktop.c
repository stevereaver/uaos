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
#include <stdint.h>
#include <stddef.h>

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
    FB_PutStr(cx, cy,        "M68k emulator: unavailable (stub build)", WB_DARK_GREY, WB_GREY);
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
    int top    = MENUBAR_H;
    int bottom = H - STATUSBAR_H;

    /* Fill base colour first */
    FB_FillRect(0, top, W, bottom - top, WB_GREY);

    /* Overlay dark dots at every other position for stipple effect */
    for (int y = top; y < bottom; y += 2)
        for (int x = y & 2 ? 0 : 1; x < W; x += 2)
            FB_PutPixel(x, y, WB_DARK_GREY);
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

    /* Centre welcome window */
    draw_welcome_window(W, H);
}

void Desktop_UpdateClock(void)
{
    /* Stub — a real implementation would read CMOS RTC and redraw clock area */
    if (!g_fb.valid) return;
}
