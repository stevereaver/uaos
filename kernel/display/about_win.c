/* about_win.c — UAOS About Window
 *
 * Workbench 3.x-style "About UAOS" window.  Displays:
 *   - UAOS logo / banner (text art)
 *   - Kernel version and build date
 *   - CPU architecture
 *   - Display resolution and depth
 *   - Memory size
 *   - Copyright notice
 *   - OK button (any click on it closes the window — raises-to-focus on re-open)
 */

#include "about_win.h"
#include "wm.h"
#include "framebuffer.h"
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Helpers (no libc)
 * ========================================================================= */

static void uint_to_dec(uint32_t v, char *buf, int max)
{
    char tmp[12];
    int  i = 0, j = 0;
    if (v == 0) { buf[j++] = '0'; buf[j] = '\0'; return; }
    while (v && i < 11) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i-- && j < max - 1) buf[j++] = tmp[i];
    buf[j] = '\0';
}

/* =========================================================================
 * Window dimensions and state
 * ========================================================================= */

#define WIN_W  400
#define WIN_H  300

static int g_wm_handle = -1;
static int g_cx = 0, g_cy = 0, g_cw = 0, g_ch = 0;  /* last-drawn client rect */

/* =========================================================================
 * Drawing helpers
 * ========================================================================= */

#define TITLEBAR_H  WM_TITLEBAR_H
#define BORDER      1   /* left/top: just outline; right uses WM scrollbar */
#define LINE_H      16
#define PAD         8

static void draw_bevel(int x, int y, int w, int h, int raised)
{
    uint32_t lo = raised ? WB_DARK_GREY : WB_WHITE;
    uint32_t hi = raised ? WB_WHITE     : WB_DARK_GREY;
    FB_DrawHLine(x,         y,         w, hi);
    FB_DrawVLine(x,         y,         h, hi);
    FB_DrawHLine(x,         y + h - 1, w, lo);
    FB_DrawVLine(x + w - 1, y,         h, lo);
}

/* Draw a text row in the client area */
static void draw_row(int cx, int *y, int cw,
                     const char *label, const char *value,
                     uint32_t label_col, uint32_t val_col, uint32_t bg)
{
    FB_PutStr(cx + PAD, *y, label, label_col, bg);

    /* Find end of label */
    int llen = 0;
    while (label[llen]) llen++;
    FB_PutStr(cx + PAD + llen * 8, *y, value, val_col, bg);
    *y += LINE_H;
}

/* =========================================================================
 * WM draw callback
 * ========================================================================= */

static void about_draw(int wx, int wy, int ww, int wh)
{
    /* ── Client area ───────────────────────────────────────── */
    int cx = wx + BORDER;
    int cy = wy + TITLEBAR_H + BORDER;
    int cw = ww - BORDER * 2;
    int ch = wh - TITLEBAR_H - BORDER * 2;
    g_cx = cx; g_cy = cy; g_cw = cw; g_ch = ch;

    FB_FillRect(cx, cy, cw, ch, WB_GREY);
    draw_bevel(cx, cy, cw, ch, 0);   /* sunken client area */

    /* ── Logo banner ───────────────────────────────────────── */
    int logo_h = 48;
    FB_FillRect(cx + 1, cy + 1, cw - 2, logo_h, WB_BLUE);

    /* UAOS large text — simulate bold by printing twice offset by 1px */
    const char *title = "Ultimate Amiga OS";
    int tw = 0; while (title[tw]) tw++;
    int tx = cx + (cw - tw * 8) / 2;
    int ty = cy + 4;
    FB_PutStr(tx + 1, ty + 1, title,   WB_DARK_GREY,  WB_BLUE);
    FB_PutStr(tx,     ty,     title,   WB_WHITE,       WB_BLUE);

    const char *sub = "UAOS Workbench 3.x-Compatible Desktop";
    int sw = 0; while (sub[sw]) sw++;
    int sx = cx + (cw - sw * 8) / 2;
    FB_PutStr(sx, ty + 20, sub, WB_CREAM, WB_BLUE);

    /* ── Separator ─────────────────────────────────────────── */
    int y = cy + logo_h + PAD;
    FB_DrawHLine(cx + PAD, y, cw - PAD * 2, WB_DARK_GREY);
    FB_DrawHLine(cx + PAD, y + 1, cw - PAD * 2, WB_WHITE);
    y += PAD + 2;

    /* ── Info rows ─────────────────────────────────────────── */
    uint32_t bg  = WB_GREY;
    uint32_t lc  = WB_DARK_GREY;   /* label colour  */
    uint32_t vc  = WB_BLACK;       /* value colour  */

    draw_row(cx, &y, cw, "Version:   ", "0.1.0-dev", lc, vc, bg);
    draw_row(cx, &y, cw, "Platform:  ", "x86_64 (bare-metal)", lc, vc, bg);
    draw_row(cx, &y, cw, "CPU:       ", "Intel/AMD 64-bit Long Mode", lc, vc, bg);

    /* Display resolution */
    char res[32];
    char num[12];
    int ri = 0, ni;
    uint_to_dec(g_fb.width, num, 12);
    for (ni = 0; num[ni]; ni++) res[ri++] = num[ni];
    res[ri++] = 'x';
    uint_to_dec(g_fb.height, num, 12);
    for (ni = 0; num[ni]; ni++) res[ri++] = num[ni];
    res[ri++] = ' '; res[ri++] = '@'; res[ri++] = ' ';
    uint_to_dec(g_fb.bpp, num, 12);
    for (ni = 0; num[ni]; ni++) res[ri++] = num[ni];
    res[ri++] = 'b'; res[ri++] = 'p'; res[ri++] = 'p'; res[ri] = '\0';
    draw_row(cx, &y, cw, "Display:   ", res, lc, vc, bg);

    draw_row(cx, &y, cw, "RAM:       ", "512 MB", lc, vc, bg);
    draw_row(cx, &y, cw, "Emulation: ", "M68k JIT thunk dispatch", lc, vc, bg);
    draw_row(cx, &y, cw, "Input:     ", "PS/2 mouse + keyboard (IRQ1/12)", lc, vc, bg);

    /* ── Separator ─────────────────────────────────────────── */
    FB_DrawHLine(cx + PAD, y, cw - PAD * 2, WB_DARK_GREY);
    FB_DrawHLine(cx + PAD, y + 1, cw - PAD * 2, WB_WHITE);
    y += PAD + 2;

    /* ── Copyright ─────────────────────────────────────────── */
    const char *copy = "Copyright 2026 UAOS Development Team";
    int cl = 0; while (copy[cl]) cl++;
    FB_PutStr(cx + (cw - cl * 8) / 2, y, copy, WB_DARK_GREY, bg);
    y += LINE_H;
    const char *lic = "Released under the AROS Public License (APL)";
    int ll = 0; while (lic[ll]) ll++;
    FB_PutStr(cx + (cw - ll * 8) / 2, y, lic, WB_DARK_GREY, bg);
    y += LINE_H + PAD;

    /* ── OK button ─────────────────────────────────────────── */
    int btn_w = 72, btn_h = 22;
    int bx = cx + (cw - btn_w) / 2;
    int by = cy + ch - btn_h - PAD;
    FB_FillRect(bx, by, btn_w, btn_h, WB_GREY);
    draw_bevel(bx, by, btn_w, btn_h, 1);
    FB_PutStrCentred(bx, by, btn_w, btn_h, "OK", WB_BLACK, WB_GREY);
}

static void about_key(char c) { (void)c; }

static void about_click(int handle, int mx, int my)
{
    (void)handle;
    int btn_w = 72, btn_h = 22;
    int bx = g_cx + (g_cw - btn_w) / 2;
    int by = g_cy + g_ch - btn_h - PAD;
    if (mx >= bx && mx < bx + btn_w && my >= by && my < by + btn_h)
        WM_CloseWindow(g_wm_handle);
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void AboutWin_Open(void)
{
    if (g_wm_handle >= 0 && WM_IsWindowActive(g_wm_handle)) {
        /* Already open — just redraw (brings it into view) */
        WM_Redraw();
        return;
    }
    g_wm_handle = -1;   /* was closed via the WM close gadget */

    /* Centre on screen */
    int sx = (int)g_fb.width;
    int sy = (int)g_fb.height;
    int wx = (sx - WIN_W) / 2;
    int wy = (sy - WIN_H) / 2;
    if (wy < WM_TITLEBAR_H + 4) wy = WM_TITLEBAR_H + 4;

    g_wm_handle = WM_AddWindow(wx, wy, WIN_W, WIN_H,
                               "About UAOS", about_draw, about_key);
    WM_SetClickHandler(g_wm_handle, about_click);
    WM_Redraw();
}
