/* prefs_win.c — UAOS Preferences Editors
 *
 * GUI editors for AmigaOS 3.x-style preferences.
 * Each editor opens a WM window with AmigaOS-look controls.
 */

#include "prefs_win.h"
#include "framebuffer.h"
#include "wm.h"
#include "../irq/rtc.h"
#include "../exec/prefs_lib.h"
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Shared UI helpers
 * ========================================================================= */

#define COL_BG       WB_GREY
#define COL_LABEL    WB_BLACK
#define COL_BORDER   WB_DARK_GREY
#define COL_BTN      WB_LIGHT_GREY
#define COL_BTN_PRS  WB_DARK_GREY
#define COL_SEL      WB_BLUE
#define COL_SEL_FG   WB_WHITE

static int pw_slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int pw_str_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* Draw a raised bevel box (AmigaOS gadget look) */
static void pw_bevel(int x, int y, int w, int h, int raised)
{
    uint32_t hi = raised ? WB_WHITE : WB_DARK_GREY;
    uint32_t lo = raised ? WB_DARK_GREY : WB_WHITE;
    FB_DrawHLine(x, y, w, hi);
    FB_DrawVLine(x, y, h, hi);
    FB_DrawHLine(x, y + h - 1, w, lo);
    FB_DrawVLine(x + w - 1, y, h, lo);
}

/* Draw a text label */
static void pw_label(int x, int y, const char *s)
{
    FB_PutStr(x, y, s, COL_LABEL, COL_BG);
}

/* Draw a button gadget */
typedef struct {
    int x, y, w, h;
    const char *label;
} PwBtn;

static void pw_btn_draw(PwBtn *b, int pressed)
{
    uint32_t bg = pressed ? COL_BTN_PRS : COL_BTN;
    FB_FillRect(b->x, b->y, b->w, b->h, bg);
    pw_bevel(b->x, b->y, b->w, b->h, !pressed);
    int tw = pw_slen(b->label) * 8;
    int th = 16;
    int tx = b->x + (b->w - tw) / 2;
    int ty = b->y + (b->h - th) / 2;
    FB_PutStr(tx, ty, b->label, COL_LABEL, bg);
}

static int pw_btn_hit(PwBtn *b, int mx, int my)
{
    return (mx >= b->x && mx < b->x + b->w &&
            my >= b->y && my < b->y + b->h);
}

/* Draw a cycle gadget (up/down arrows + current value) */
typedef struct {
    int x, y, w, h;
    const char *value;
} PwCycle;

static void pw_cycle_draw(PwCycle *c)
{
    FB_FillRect(c->x, c->y, c->w, c->h, COL_BTN);
    pw_bevel(c->x, c->y, c->w, c->h, 1);
    /* Value text */
    int tw = pw_slen(c->value) * 8;
    int tx = c->x + 4;
    int ty = c->y + (c->h - 16) / 2;
    FB_PutStr(tx, ty, c->value, COL_LABEL, COL_BTN);
    /* Arrow buttons on right */
    int ax = c->x + c->w - 18;
    int ah = c->h / 2;
    FB_FillRect(ax, c->y, 16, ah, COL_BTN);
    pw_bevel(ax, c->y, 16, ah, 1);
    FB_PutStr(ax + 4, c->y + (ah - 16) / 2, "^", COL_LABEL, COL_BTN);
    FB_FillRect(ax, c->y + ah, 16, ah, COL_BTN);
    pw_bevel(ax, c->y + ah, 16, ah, 1);
    FB_PutStr(ax + 4, c->y + ah + (ah - 16) / 2, "v", COL_LABEL, COL_BTN);
}

static int pw_cycle_hit_up(PwCycle *c, int mx, int my)
{
    int ax = c->x + c->w - 18;
    int ah = c->h / 2;
    return (mx >= ax && mx < ax + 16 && my >= c->y && my < c->y + ah);
}

static int pw_cycle_hit_dn(PwCycle *c, int mx, int my)
{
    int ax = c->x + c->w - 18;
    int ah = c->h / 2;
    return (mx >= ax && mx < ax + 16 && my >= c->y + ah && my < c->y + c->h);
}

/* Draw a slider gadget (horizontal) */
typedef struct {
    int x, y, w, h;
    int min, max, val;
} PwSlider;

static void pw_slider_draw(PwSlider *s)
{
    /* Track */
    int track_y = s->y + (s->h - 6) / 2;
    FB_FillRect(s->x, track_y, s->w, 6, WB_WHITE);
    pw_bevel(s->x, track_y, s->w, 6, 0);
    /* Knob */
    int range = s->max - s->min;
    int kx;
    if (range <= 0) kx = s->x;
    else kx = s->x + ((s->val - s->min) * (s->w - 16)) / range;
    int kw = 16;
    FB_FillRect(kx, s->y, kw, s->h, COL_BTN);
    pw_bevel(kx, s->y, kw, s->h, 1);
}

static int pw_slider_hit(PwSlider *s, int mx, int my)
{
    return (mx >= s->x && mx < s->x + s->w &&
            my >= s->y && my < s->y + s->h);
}

static void pw_slider_set_from_mouse(PwSlider *s, int mx)
{
    int range = s->max - s->min;
    if (range <= 0) { s->val = s->min; return; }
    int rel = mx - s->x - 8;
    if (rel < 0) rel = 0;
    if (rel > s->w - 16) rel = s->w - 16;
    s->val = s->min + (rel * range) / (s->w - 16);
}

/* Draw a check box gadget */
typedef struct {
    int x, y;
    const char *label;
    int checked;
} PwCheck;

static void pw_check_draw(PwCheck *c)
{
    FB_FillRect(c->x, c->y, 14, 14, WB_WHITE);
    pw_bevel(c->x, c->y, 14, 14, 0);
    if (c->checked) {
        FB_PutStr(c->x + 2, c->y - 1, "X", COL_LABEL, WB_WHITE);
    }
    FB_PutStr(c->x + 20, c->y - 1, c->label, COL_LABEL, COL_BG);
}

static int pw_check_hit(PwCheck *c, int mx, int my)
{
    return (mx >= c->x && mx < c->x + 14 + pw_slen(c->label) * 8 + 20 &&
            my >= c->y && my < c->y + 14);
}

/* Standard button row at bottom of a prefs window */
#define PW_BTN_W  80
#define PW_BTN_H  22
#define PW_BTN_GAP 12

static void pw_btn_row(PwBtn *apply, PwBtn *save, PwBtn *close,
                       int wx, int wy, int ww, int wh)
{
    int y = wy + wh - WM_TITLEBAR_H - PW_BTN_H - 8;
    int total = PW_BTN_W * 3 + PW_BTN_GAP * 2;
    int x = wx + (ww - total) / 2;
    apply->x = x; apply->y = y; apply->w = PW_BTN_W; apply->h = PW_BTN_H; apply->label = "Apply";
    x += PW_BTN_W + PW_BTN_GAP;
    save->x = x; save->y = y; save->w = PW_BTN_W; save->h = PW_BTN_H; save->label = "Save";
    x += PW_BTN_W + PW_BTN_GAP;
    close->x = x; close->y = y; close->w = PW_BTN_W; close->h = PW_BTN_H; close->label = "Close";
}

static void pw_draw_btn_row(PwBtn *apply, PwBtn *save, PwBtn *close)
{
    pw_btn_draw(apply, 0);
    pw_btn_draw(save, 0);
    pw_btn_draw(close, 0);
}

static void pw_draw_bg(int x, int y, int w, int h)
{
    FB_FillRect(x, y + WM_TITLEBAR_H, w, h - WM_TITLEBAR_H, COL_BG);
}

/* Int to string */
static void pw_int_str(char *buf, int val)
{
    int i = 0;
    if (val < 0) { buf[i++] = '-'; val = -val; }
    char tmp[12];
    int j = 0;
    if (val == 0) tmp[j++] = '0';
    while (val > 0) { tmp[j++] = '0' + (val % 10); val /= 10; }
    while (j > 0) buf[i++] = tmp[--j];
    buf[i] = '\0';
}

/* =========================================================================
 * Palette Preferences
 * ========================================================================= */

#define PAL_WIN_W  420
#define PAL_WIN_H  340

static int g_pal_handle = -1;

/* Palette state: 8 colors, each with R,G,B (0-255) */
typedef struct { uint8_t r, g, b; } PalColor;

static const char *pal_names[8] = {
    "Grey", "Light Grey", "Dark Grey", "Black",
    "White", "Blue", "Orange", "Cream"
};

/* Default WB palette (matches framebuffer.c WB_InitPalette) */
static const PalColor pal_defaults[8] = {
    {0xAA, 0xAA, 0xAA},  /* WB_GREY       */
    {0xCC, 0xCC, 0xCC},  /* WB_LIGHT_GREY */
    {0x55, 0x55, 0x55},  /* WB_DARK_GREY  */
    {0x00, 0x00, 0x00},  /* WB_BLACK      */
    {0xFF, 0xFF, 0xFF},  /* WB_WHITE      */
    {0x55, 0x88, 0xBB},  /* WB_BLUE       */
    {0xDD, 0x88, 0x00},  /* WB_ORANGE     */
    {0xFF, 0xEE, 0xBB},  /* WB_CREAM      */
};

static PalColor g_pal_colors[8];
static int g_pal_sel = 0;  /* selected color index */
static PwSlider g_pal_r, g_pal_g, g_pal_b;
static PwBtn g_pal_apply, g_pal_save, g_pal_close;

static void pal_load_current(void)
{
    g_pal_colors[0].r = 0xAA; g_pal_colors[0].g = 0xAA; g_pal_colors[0].b = 0xAA;
    /* Extract from current WB_* values */
    uint32_t *wb_ptrs[8] = {&WB_GREY, &WB_LIGHT_GREY, &WB_DARK_GREY, &WB_BLACK,
                             &WB_WHITE, &WB_BLUE, &WB_ORANGE, &WB_CREAM};
    for (int i = 0; i < 8; i++) {
        uint32_t v = *wb_ptrs[i];
        g_pal_colors[i].r = (v >> 16) & 0xFF;
        g_pal_colors[i].g = (v >> 8) & 0xFF;
        g_pal_colors[i].b = v & 0xFF;
    }
}

static void pal_apply_colors(void)
{
    WB_GREY       = FB_RGB(g_pal_colors[0].r, g_pal_colors[0].g, g_pal_colors[0].b);
    WB_LIGHT_GREY = FB_RGB(g_pal_colors[1].r, g_pal_colors[1].g, g_pal_colors[1].b);
    WB_DARK_GREY  = FB_RGB(g_pal_colors[2].r, g_pal_colors[2].g, g_pal_colors[2].b);
    WB_BLACK      = FB_RGB(g_pal_colors[3].r, g_pal_colors[3].g, g_pal_colors[3].b);
    WB_WHITE      = FB_RGB(g_pal_colors[4].r, g_pal_colors[4].g, g_pal_colors[4].b);
    WB_BLUE       = FB_RGB(g_pal_colors[5].r, g_pal_colors[5].g, g_pal_colors[5].b);
    WB_ORANGE     = FB_RGB(g_pal_colors[6].r, g_pal_colors[6].g, g_pal_colors[6].b);
    WB_CREAM      = FB_RGB(g_pal_colors[7].r, g_pal_colors[7].g, g_pal_colors[7].b);
}

static void pal_save_prefs(void)
{
    PrefsFile pf;
    memset(&pf, 0, sizeof(pf));
    Prefs_SetType(&pf, PREFS_PALETTE);

    /* PLRM chunk: 8 colors x 3 bytes (R,G,B) = 24 bytes */
    uint8_t data[24];
    for (int i = 0; i < 8; i++) {
        data[i * 3]     = g_pal_colors[i].r;
        data[i * 3 + 1] = g_pal_colors[i].g;
        data[i * 3 + 2] = g_pal_colors[i].b;
    }
    Prefs_SetChunk(&pf, "PLRM", data, 24);
    Prefs_Save("ENVARC:Sys/palette.prefs", &pf);
    Prefs_Save("ENV:Sys/palette.prefs", &pf);
}

static void pal_load_prefs(void)
{
    PrefsFile pf;
    if (!Prefs_Load("ENVARC:Sys/palette.prefs", &pf)) return;
    const PrefsChunk *ch = Prefs_FindChunk(&pf, "PLRM");
    if (ch && ch->size >= 24) {
        for (int i = 0; i < 8; i++) {
            g_pal_colors[i].r = ch->data[i * 3];
            g_pal_colors[i].g = ch->data[i * 3 + 1];
            g_pal_colors[i].b = ch->data[i * 3 + 2];
        }
    }
}

static void pal_draw(int wx, int wy, int ww, int wh)
{
    pw_draw_bg(wx, wy, ww, wh);

    /* Color list on left */
    int lx = wx + 10;
    int ly = wy + WM_TITLEBAR_H + 10;
    pw_label(lx, ly, "Colors:");
    ly += 20;

    for (int i = 0; i < 8; i++) {
        int ry = ly + i * 22;
        /* Selection highlight */
        if (i == g_pal_sel) {
            FB_FillRect(lx - 2, ry - 2, 120, 20, COL_SEL);
            FB_PutStr(lx, ry, pal_names[i], COL_SEL_FG, COL_SEL);
        } else {
            FB_PutStr(lx, ry, pal_names[i], COL_LABEL, COL_BG);
        }
        /* Color swatch */
        uint32_t col = FB_RGB(g_pal_colors[i].r, g_pal_colors[i].g, g_pal_colors[i].b);
        FB_FillRect(lx + 90, ry, 20, 16, col);
        pw_bevel(lx + 90, ry, 20, 16, 1);
    }

    /* Sliders on right */
    int sx = wx + 150;
    int sy = wy + WM_TITLEBAR_H + 30;
    int sw = 240;

    /* Selected color name + preview */
    pw_label(sx, sy - 20, "Editing:");
    FB_PutStr(sx + 64, sy - 20, pal_names[g_pal_sel], COL_LABEL, COL_BG);
    uint32_t prev = FB_RGB(g_pal_colors[g_pal_sel].r, g_pal_colors[g_pal_sel].g, g_pal_colors[g_pal_sel].b);
    FB_FillRect(sx + sw - 40, sy - 24, 40, 20, prev);
    pw_bevel(sx + sw - 40, sy - 24, 40, 20, 1);

    /* R slider */
    pw_label(sx, sy, "Red:");
    g_pal_r.x = sx + 50; g_pal_r.y = sy - 4; g_pal_r.w = sw - 50; g_pal_r.h = 20;
    g_pal_r.min = 0; g_pal_r.max = 255; g_pal_r.val = g_pal_colors[g_pal_sel].r;
    pw_slider_draw(&g_pal_r);

    /* G slider */
    sy += 30;
    pw_label(sx, sy, "Green:");
    g_pal_g.x = sx + 50; g_pal_g.y = sy - 4; g_pal_g.w = sw - 50; g_pal_g.h = 20;
    g_pal_g.min = 0; g_pal_g.max = 255; g_pal_g.val = g_pal_colors[g_pal_sel].g;
    pw_slider_draw(&g_pal_g);

    /* B slider */
    sy += 30;
    pw_label(sx, sy, "Blue:");
    g_pal_b.x = sx + 50; g_pal_b.y = sy - 4; g_pal_b.w = sw - 50; g_pal_b.h = 20;
    g_pal_b.min = 0; g_pal_b.max = 255; g_pal_b.val = g_pal_colors[g_pal_sel].b;
    pw_slider_draw(&g_pal_b);

    /* RGB values */
    sy += 30;
    char buf[16];
    pw_int_str(buf, g_pal_colors[g_pal_sel].r);
    FB_PutStr(sx, sy, "R:", COL_LABEL, COL_BG); FB_PutStr(sx + 16, sy, buf, COL_LABEL, COL_BG);
    pw_int_str(buf, g_pal_colors[g_pal_sel].g);
    FB_PutStr(sx + 60, sy, "G:", COL_LABEL, COL_BG); FB_PutStr(sx + 76, sy, buf, COL_LABEL, COL_BG);
    pw_int_str(buf, g_pal_colors[g_pal_sel].b);
    FB_PutStr(sx + 120, sy, "B:", COL_LABEL, COL_BG); FB_PutStr(sx + 136, sy, buf, COL_LABEL, COL_BG);

    /* Buttons */
    pw_btn_row(&g_pal_apply, &g_pal_save, &g_pal_close, wx, wy, ww, wh);
    pw_draw_btn_row(&g_pal_apply, &g_pal_save, &g_pal_close);
}

static void pal_key(char c)
{
    if (c == 27) { /* ESC */
        WM_CloseWindow(g_pal_handle);
        g_pal_handle = -1;
    }
}

static int g_pal_drag_slider = 0; /* 0=none, 1=R, 2=G, 3=B */

static void pal_click(int handle, int mx, int my)
{
    (void)handle;

    /* Color list clicks */
    int wwx, wwy, www, wwh;
    WM_GetWindowRect(g_pal_handle, &wwx, &wwy, &www, &wwh);
    int lx = wwx + 10;
    int ly = wwy + WM_TITLEBAR_H + 30;
    for (int i = 0; i < 8; i++) {
        int ry = ly + i * 22;
        if (mx >= lx - 2 && mx < lx + 120 && my >= ry - 2 && my < ry + 18) {
            g_pal_sel = i;
            WM_Redraw();
            return;
        }
    }

    /* Slider clicks */
    if (pw_slider_hit(&g_pal_r, mx, my)) { g_pal_drag_slider = 1; pw_slider_set_from_mouse(&g_pal_r, mx); g_pal_colors[g_pal_sel].r = g_pal_r.val; WM_Redraw(); return; }
    if (pw_slider_hit(&g_pal_g, mx, my)) { g_pal_drag_slider = 2; pw_slider_set_from_mouse(&g_pal_g, mx); g_pal_colors[g_pal_sel].g = g_pal_g.val; WM_Redraw(); return; }
    if (pw_slider_hit(&g_pal_b, mx, my)) { g_pal_drag_slider = 3; pw_slider_set_from_mouse(&g_pal_b, mx); g_pal_colors[g_pal_sel].b = g_pal_b.val; WM_Redraw(); return; }

    /* Buttons */
    if (pw_btn_hit(&g_pal_apply, mx, my)) { pal_apply_colors(); WM_Redraw(); return; }
    if (pw_btn_hit(&g_pal_save, mx, my)) { pal_apply_colors(); pal_save_prefs(); WM_Redraw(); return; }
    if (pw_btn_hit(&g_pal_close, mx, my)) { WM_CloseWindow(g_pal_handle); g_pal_handle = -1; return; }
}

static void pal_mouse_move(int handle, int mx, int my)
{
    (void)handle; (void)my;
    if (g_pal_drag_slider == 1) { pw_slider_set_from_mouse(&g_pal_r, mx); g_pal_colors[g_pal_sel].r = g_pal_r.val; WM_Redraw(); }
    else if (g_pal_drag_slider == 2) { pw_slider_set_from_mouse(&g_pal_g, mx); g_pal_colors[g_pal_sel].g = g_pal_g.val; WM_Redraw(); }
    else if (g_pal_drag_slider == 3) { pw_slider_set_from_mouse(&g_pal_b, mx); g_pal_colors[g_pal_sel].b = g_pal_b.val; WM_Redraw(); }
}

static void pal_mouse_release(int handle, int mx, int my)
{
    (void)handle; (void)mx; (void)my;
    g_pal_drag_slider = 0;
}

void PalettePrefs_Show(void)
{
    if (g_pal_handle >= 0 && WM_GetDrawFn(g_pal_handle) == pal_draw) {
        WM_RaiseWindow(g_pal_handle);
        WM_Redraw();
        return;
    }
    g_pal_handle = -1;

    pal_load_current();
    pal_load_prefs();

    int wx = ((int)g_fb.width - PAL_WIN_W) / 2;
    int wy = ((int)g_fb.height - PAL_WIN_H) / 2;
    if (wy < WM_TITLEBAR_H + 4) wy = WM_TITLEBAR_H + 4;

    g_pal_handle = WM_AddWindow(wx, wy, PAL_WIN_W, PAL_WIN_H, "Palette Prefs", pal_draw, pal_key);
    if (g_pal_handle >= 0) {
        WM_SetClickHandler(g_pal_handle, pal_click);
        WM_SetMouseMoveHandler(g_pal_handle, pal_mouse_move);
        WM_SetMouseReleaseHandler(g_pal_handle, pal_mouse_release);
        WM_Redraw();
    }
}

/* =========================================================================
 * Time Preferences
 * ========================================================================= */

#define TIME_WIN_W  340
#define TIME_WIN_H  260

static int g_time_handle = -1;
static RtcDateTime g_time_dt;
static PwBtn g_time_set, g_time_close;

/* Which field is selected for editing: 0=year 1=month 2=day 3=hour 4=min 5=sec */
static int g_time_field = 0;

static const char *month_names[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static void time_draw(int wx, int wy, int ww, int wh)
{
    pw_draw_bg(wx, wy, ww, wh);

    int cx = wx + 20;
    int cy = wy + WM_TITLEBAR_H + 20;

    pw_label(cx, cy, "Date & Time Settings");
    cy += 30;

    /* Field labels and values */
    const char *labels[] = {"Year:", "Month:", "Day:", "Hour:", "Min:", "Sec:"};
    int values[6];
    values[0] = g_time_dt.year;
    values[1] = g_time_dt.month;
    values[2] = g_time_dt.day;
    values[3] = g_time_dt.hour;
    values[4] = g_time_dt.min;
    values[5] = g_time_dt.sec;

    for (int i = 0; i < 6; i++) {
        int fy = cy + i * 24;
        /* Label */
        pw_label(cx, fy, labels[i]);

        /* Value box */
        int bx = cx + 60;
        int bw = 100;
        int bh = 18;
        if (i == g_time_field) {
            FB_FillRect(bx, fy - 2, bw, bh, COL_SEL);
            pw_bevel(bx, fy - 2, bw, bh, 1);
        } else {
            FB_FillRect(bx, fy - 2, bw, bh, WB_WHITE);
            pw_bevel(bx, fy - 2, bw, bh, 0);
        }

        char buf[16];
        if (i == 1) {
            /* Month name */
            int mi = values[i] - 1;
            if (mi < 0) mi = 0;
            if (mi > 11) mi = 11;
            buf[0] = month_names[mi][0];
            buf[1] = month_names[mi][1];
            buf[2] = month_names[mi][2];
            buf[3] = '\0';
        } else if (i == 0) {
            pw_int_str(buf, values[i]);
        } else {
            if (values[i] < 10) { buf[0] = '0'; buf[1] = '0' + values[i]; buf[2] = '\0'; }
            else pw_int_str(buf, values[i]);
        }

        uint32_t fg = (i == g_time_field) ? COL_SEL_FG : COL_LABEL;
        uint32_t bg = (i == g_time_field) ? COL_SEL : WB_WHITE;
        FB_PutStr(bx + 8, fy, buf, fg, bg);

        /* +/- buttons */
        int upx = bx + bw + 6;
        FB_FillRect(upx, fy - 2, 20, 9, COL_BTN);
        pw_bevel(upx, fy - 2, 20, 9, 1);
        FB_PutStr(upx + 6, fy - 3, "+", COL_LABEL, COL_BTN);
        FB_FillRect(upx, fy + 7, 20, 9, COL_BTN);
        pw_bevel(upx, fy + 7, 20, 9, 1);
        FB_PutStr(upx + 6, fy + 5, "-", COL_LABEL, COL_BTN);
    }

    /* Buttons */
    g_time_set.x = wx + 60; g_time_set.y = wy + wh - WM_TITLEBAR_H - 30;
    g_time_set.w = PW_BTN_W; g_time_set.h = PW_BTN_H; g_time_set.label = "Set Time";
    g_time_close.x = wx + ww - PW_BTN_W - 60; g_time_close.y = g_time_set.y;
    g_time_close.w = PW_BTN_W; g_time_close.h = PW_BTN_H; g_time_close.label = "Close";
    pw_btn_draw(&g_time_set, 0);
    pw_btn_draw(&g_time_close, 0);
}

static void time_adjust_field(int field, int delta)
{
    switch (field) {
        case 0: /* Year */
            g_time_dt.year += delta;
            if (g_time_dt.year < 1970) g_time_dt.year = 1970;
            if (g_time_dt.year > 2099) g_time_dt.year = 2099;
            break;
        case 1: /* Month */
            g_time_dt.month += delta;
            if (g_time_dt.month < 1) g_time_dt.month = 12;
            if (g_time_dt.month > 12) g_time_dt.month = 1;
            break;
        case 2: /* Day */
            g_time_dt.day += delta;
            if (g_time_dt.day < 1) g_time_dt.day = 31;
            if (g_time_dt.day > 31) g_time_dt.day = 1;
            break;
        case 3: /* Hour */
            g_time_dt.hour += delta;
            if (g_time_dt.hour > 23) g_time_dt.hour = 0;
            if (g_time_dt.hour < 0) g_time_dt.hour = 23;
            break;
        case 4: /* Min */
            g_time_dt.min += delta;
            if (g_time_dt.min > 59) g_time_dt.min = 0;
            if (g_time_dt.min < 0) g_time_dt.min = 59;
            break;
        case 5: /* Sec */
            g_time_dt.sec += delta;
            if (g_time_dt.sec > 59) g_time_dt.sec = 0;
            if (g_time_dt.sec < 0) g_time_dt.sec = 59;
            break;
    }
}

static void time_key(char c)
{
    if (c == 27) { /* ESC */
        WM_CloseWindow(g_time_handle);
        g_time_handle = -1;
        return;
    }
    if (c == 9) { /* Tab - next field */
        g_time_field = (g_time_field + 1) % 6;
        WM_Redraw();
        return;
    }
    if (c == '+' || c == '=') { time_adjust_field(g_time_field, 1); WM_Redraw(); return; }
    if (c == '-') { time_adjust_field(g_time_field, -1); WM_Redraw(); return; }
}

static void time_click(int handle, int mx, int my)
{
    (void)handle;
    int wwx, wwy, www, wwh;
    WM_GetWindowRect(g_time_handle, &wwx, &wwy, &www, &wwh);
    int cx = wwx + 20;
    int cy = wwy + WM_TITLEBAR_H + 50;

    for (int i = 0; i < 6; i++) {
        int fy = cy + i * 24;
        int bx = cx + 60;
        int bw = 100;

        /* Field click - select */
        if (mx >= bx && mx < bx + bw && my >= fy - 2 && my < fy + 16) {
            g_time_field = i;
            WM_Redraw();
            return;
        }

        /* + button */
        int upx = bx + bw + 6;
        if (mx >= upx && mx < upx + 20 && my >= fy - 2 && my < fy + 7) {
            time_adjust_field(i, 1);
            g_time_field = i;
            WM_Redraw();
            return;
        }
        /* - button */
        if (mx >= upx && mx < upx + 20 && my >= fy + 7 && my < fy + 16) {
            time_adjust_field(i, -1);
            g_time_field = i;
            WM_Redraw();
            return;
        }
    }

    /* Set button */
    if (pw_btn_hit(&g_time_set, mx, my)) {
        RTC_SetDateTime(&g_time_dt);
        WM_Redraw();
        return;
    }
    /* Close button */
    if (pw_btn_hit(&g_time_close, mx, my)) {
        WM_CloseWindow(g_time_handle);
        g_time_handle = -1;
        return;
    }
}

void TimePrefs_Show(void)
{
    if (g_time_handle >= 0 && WM_GetDrawFn(g_time_handle) == time_draw) {
        WM_RaiseWindow(g_time_handle);
        WM_Redraw();
        return;
    }
    g_time_handle = -1;

    g_time_dt = RTC_ReadDateTime();

    int wx = ((int)g_fb.width - TIME_WIN_W) / 2;
    int wy = ((int)g_fb.height - TIME_WIN_H) / 2;
    if (wy < WM_TITLEBAR_H + 4) wy = WM_TITLEBAR_H + 4;

    g_time_handle = WM_AddWindow(wx, wy, TIME_WIN_W, TIME_WIN_H, "Time Prefs", time_draw, time_key);
    if (g_time_handle >= 0) {
        WM_SetClickHandler(g_time_handle, time_click);
        WM_Redraw();
    }
}

/* =========================================================================
 * IControl Preferences
 * ========================================================================= */

#define IC_WIN_W  360
#define IC_WIN_H  280

static int g_ic_handle = -1;
static PwCheck g_ic_click_front, g_ic_menu_popup, g_ic_boopsi;
static PwSlider g_ic_timeout;
static PwBtn g_ic_apply, g_ic_save, g_ic_close;

static void ic_draw(int wx, int wy, int ww, int wh)
{
    pw_draw_bg(wx, wy, ww, wh);

    int cx = wx + 16;
    int cy = wy + WM_TITLEBAR_H + 16;

    pw_label(cx, cy, "Intuition Control Preferences");
    cy += 28;

    g_ic_click_front.x = cx; g_ic_click_front.y = cy;
    g_ic_click_front.label = "Click to front";
    g_ic_click_front.checked = 1;
    pw_check_draw(&g_ic_click_front);
    cy += 24;

    g_ic_menu_popup.x = cx; g_ic_menu_popup.y = cy;
    g_ic_menu_popup.label = "Menu popup on press";
    g_ic_menu_popup.checked = 1;
    pw_check_draw(&g_ic_menu_popup);
    cy += 24;

    g_ic_boopsi.x = cx; g_ic_boopsi.y = cy;
    g_ic_boopsi.label = "Boopsi menus";
    g_ic_boopsi.checked = 0;
    pw_check_draw(&g_ic_boopsi);
    cy += 30;

    pw_label(cx, cy, "Input timeout:");
    g_ic_timeout.x = cx + 120; g_ic_timeout.y = cy - 4;
    g_ic_timeout.w = 180; g_ic_timeout.h = 20;
    g_ic_timeout.min = 0; g_ic_timeout.max = 50; g_ic_timeout.val = 10;
    pw_slider_draw(&g_ic_timeout);

    pw_btn_row(&g_ic_apply, &g_ic_save, &g_ic_close, wx, wy, ww, wh);
    pw_draw_btn_row(&g_ic_apply, &g_ic_save, &g_ic_close);
}

static void ic_key(char c)
{
    if (c == 27) { WM_CloseWindow(g_ic_handle); g_ic_handle = -1; }
}

static void ic_click(int handle, int mx, int my)
{
    (void)handle;
    if (pw_check_hit(&g_ic_click_front, mx, my)) { g_ic_click_front.checked ^= 1; WM_Redraw(); return; }
    if (pw_check_hit(&g_ic_menu_popup, mx, my)) { g_ic_menu_popup.checked ^= 1; WM_Redraw(); return; }
    if (pw_check_hit(&g_ic_boopsi, mx, my)) { g_ic_boopsi.checked ^= 1; WM_Redraw(); return; }
    if (pw_slider_hit(&g_ic_timeout, mx, my)) { pw_slider_set_from_mouse(&g_ic_timeout, mx); WM_Redraw(); return; }
    if (pw_btn_hit(&g_ic_apply, mx, my)) { WM_Redraw(); return; }
    if (pw_btn_hit(&g_ic_save, mx, my)) { WM_Redraw(); return; }
    if (pw_btn_hit(&g_ic_close, mx, my)) { WM_CloseWindow(g_ic_handle); g_ic_handle = -1; return; }
}

void IControlPrefs_Show(void)
{
    if (g_ic_handle >= 0 && WM_GetDrawFn(g_ic_handle) == ic_draw) {
        WM_RaiseWindow(g_ic_handle); WM_Redraw(); return;
    }
    g_ic_handle = -1;
    int wx = ((int)g_fb.width - IC_WIN_W) / 2;
    int wy = ((int)g_fb.height - IC_WIN_H) / 2;
    if (wy < WM_TITLEBAR_H + 4) wy = WM_TITLEBAR_H + 4;
    g_ic_handle = WM_AddWindow(wx, wy, IC_WIN_W, IC_WIN_H, "IControl Prefs", ic_draw, ic_key);
    if (g_ic_handle >= 0) { WM_SetClickHandler(g_ic_handle, ic_click); WM_Redraw(); }
}

/* =========================================================================
 * Input Preferences
 * ========================================================================= */

#define IN_WIN_W  360
#define IN_WIN_H  260

static int g_in_handle = -1;
static PwSlider g_in_accel, g_in_repeat_rate, g_in_repeat_delay;
static PwBtn g_in_apply, g_in_save, g_in_close;

static void in_draw(int wx, int wy, int ww, int wh)
{
    pw_draw_bg(wx, wy, ww, wh);
    int cx = wx + 16;
    int cy = wy + WM_TITLEBAR_H + 16;

    pw_label(cx, cy, "Input Preferences");
    cy += 30;

    pw_label(cx, cy, "Mouse Acceleration:");
    g_in_accel.x = cx + 160; g_in_accel.y = cy - 4; g_in_accel.w = 160; g_in_accel.h = 20;
    g_in_accel.min = 0; g_in_accel.max = 100; g_in_accel.val = 50;
    pw_slider_draw(&g_in_accel);
    cy += 30;

    pw_label(cx, cy, "Key Repeat Rate:");
    g_in_repeat_rate.x = cx + 160; g_in_repeat_rate.y = cy - 4; g_in_repeat_rate.w = 160; g_in_repeat_rate.h = 20;
    g_in_repeat_rate.min = 0; g_in_repeat_rate.max = 10; g_in_repeat_rate.val = 4;
    pw_slider_draw(&g_in_repeat_rate);
    cy += 30;

    pw_label(cx, cy, "Key Repeat Delay:");
    g_in_repeat_delay.x = cx + 160; g_in_repeat_delay.y = cy - 4; g_in_repeat_delay.w = 160; g_in_repeat_delay.h = 20;
    g_in_repeat_delay.min = 0; g_in_repeat_delay.max = 50; g_in_repeat_delay.val = 20;
    pw_slider_draw(&g_in_repeat_delay);

    pw_btn_row(&g_in_apply, &g_in_save, &g_in_close, wx, wy, ww, wh);
    pw_draw_btn_row(&g_in_apply, &g_in_save, &g_in_close);
}

static void in_key(char c)
{
    if (c == 27) { WM_CloseWindow(g_in_handle); g_in_handle = -1; }
}

static void in_click(int handle, int mx, int my)
{
    (void)handle;
    if (pw_slider_hit(&g_in_accel, mx, my)) { pw_slider_set_from_mouse(&g_in_accel, mx); WM_Redraw(); return; }
    if (pw_slider_hit(&g_in_repeat_rate, mx, my)) { pw_slider_set_from_mouse(&g_in_repeat_rate, mx); WM_Redraw(); return; }
    if (pw_slider_hit(&g_in_repeat_delay, mx, my)) { pw_slider_set_from_mouse(&g_in_repeat_delay, mx); WM_Redraw(); return; }
    if (pw_btn_hit(&g_in_apply, mx, my)) { WM_Redraw(); return; }
    if (pw_btn_hit(&g_in_save, mx, my)) { WM_Redraw(); return; }
    if (pw_btn_hit(&g_in_close, mx, my)) { WM_CloseWindow(g_in_handle); g_in_handle = -1; return; }
}

void InputPrefs_Show(void)
{
    if (g_in_handle >= 0 && WM_GetDrawFn(g_in_handle) == in_draw) {
        WM_RaiseWindow(g_in_handle); WM_Redraw(); return;
    }
    g_in_handle = -1;
    int wx = ((int)g_fb.width - IN_WIN_W) / 2;
    int wy = ((int)g_fb.height - IN_WIN_H) / 2;
    if (wy < WM_TITLEBAR_H + 4) wy = WM_TITLEBAR_H + 4;
    g_in_handle = WM_AddWindow(wx, wy, IN_WIN_W, IN_WIN_H, "Input Prefs", in_draw, in_key);
    if (g_in_handle >= 0) { WM_SetClickHandler(g_in_handle, in_click); WM_Redraw(); }
}

/* =========================================================================
 * ScreenMode Preferences
 * ========================================================================= */

#define SM_WIN_W  380
#define SM_WIN_H  280

static int g_sm_handle = -1;
static PwCycle g_sm_mode;
static PwBtn g_sm_apply, g_sm_close;
static int g_sm_mode_idx = 0;

static const char *sm_modes[] = {
    "VBE 1024x768 24-bit",
    "VBE 800x600 24-bit",
    "VBE 640x480 24-bit",
};
#define SM_MODE_COUNT 3

static void sm_draw(int wx, int wy, int ww, int wh)
{
    pw_draw_bg(wx, wy, ww, wh);
    int cx = wx + 16;
    int cy = wy + WM_TITLEBAR_H + 16;

    pw_label(cx, cy, "Screen Mode Preferences");
    cy += 30;

    pw_label(cx, cy, "Display Mode:");
    g_sm_mode.x = cx + 110; g_sm_mode.y = cy - 2; g_sm_mode.w = 220; g_sm_mode.h = 20;
    g_sm_mode.value = sm_modes[g_sm_mode_idx];
    pw_cycle_draw(&g_sm_mode);
    cy += 40;

    /* Info display */
    pw_label(cx, cy, "Current Resolution:");
    char buf[32];
    pw_int_str(buf, (int)g_fb.width);
    FB_PutStr(cx + 130, cy, buf, COL_LABEL, COL_BG);
    FB_PutStr(cx + 130 + pw_slen(buf) * 8 + 8, cy, "x", COL_LABEL, COL_BG);
    pw_int_str(buf, (int)g_fb.height);
    FB_PutStr(cx + 130 + pw_slen(buf) * 8 + 24, cy, buf, COL_LABEL, COL_BG);
    cy += 24;

    pw_label(cx, cy, "Color Depth:");
    pw_int_str(buf, g_fb.bpp);
    FB_PutStr(cx + 130, cy, buf, COL_LABEL, COL_BG);
    FB_PutStr(cx + 130 + pw_slen(buf) * 8, cy, "-bit", COL_LABEL, COL_BG);

    /* Buttons */
    g_sm_apply.x = wx + 80; g_sm_apply.y = wy + wh - WM_TITLEBAR_H - 30;
    g_sm_apply.w = PW_BTN_W; g_sm_apply.h = PW_BTN_H; g_sm_apply.label = "Apply";
    g_sm_close.x = wx + ww - PW_BTN_W - 80; g_sm_close.y = g_sm_apply.y;
    g_sm_close.w = PW_BTN_W; g_sm_close.h = PW_BTN_H; g_sm_close.label = "Close";
    pw_btn_draw(&g_sm_apply, 0);
    pw_btn_draw(&g_sm_close, 0);
}

static void sm_key(char c)
{
    if (c == 27) { WM_CloseWindow(g_sm_handle); g_sm_handle = -1; }
}

static void sm_click(int handle, int mx, int my)
{
    (void)handle;
    if (pw_cycle_hit_up(&g_sm_mode, mx, my)) {
        g_sm_mode_idx = (g_sm_mode_idx + 1) % SM_MODE_COUNT;
        WM_Redraw(); return;
    }
    if (pw_cycle_hit_dn(&g_sm_mode, mx, my)) {
        g_sm_mode_idx = (g_sm_mode_idx - 1 + SM_MODE_COUNT) % SM_MODE_COUNT;
        WM_Redraw(); return;
    }
    if (pw_btn_hit(&g_sm_apply, mx, my)) { WM_Redraw(); return; }
    if (pw_btn_hit(&g_sm_close, mx, my)) { WM_CloseWindow(g_sm_handle); g_sm_handle = -1; return; }
}

void ScreenModePrefs_Show(void)
{
    if (g_sm_handle >= 0 && WM_GetDrawFn(g_sm_handle) == sm_draw) {
        WM_RaiseWindow(g_sm_handle); WM_Redraw(); return;
    }
    g_sm_handle = -1;
    int wx = ((int)g_fb.width - SM_WIN_W) / 2;
    int wy = ((int)g_fb.height - SM_WIN_H) / 2;
    if (wy < WM_TITLEBAR_H + 4) wy = WM_TITLEBAR_H + 4;
    g_sm_handle = WM_AddWindow(wx, wy, SM_WIN_W, SM_WIN_H, "ScreenMode Prefs", sm_draw, sm_key);
    if (g_sm_handle >= 0) { WM_SetClickHandler(g_sm_handle, sm_click); WM_Redraw(); }
}

/* =========================================================================
 * WBPattern Preferences
 * ========================================================================= */

#define WP_WIN_W  360
#define WP_WIN_H  260

static int g_wp_handle = -1;
static PwCycle g_wp_pattern;
static PwBtn g_wp_apply, g_wp_save, g_wp_close;
static int g_wp_pat_idx = 0;

static const char *wp_patterns[] = {
    "Solid Grey",
    "Checkerboard",
    "Dots",
    "Brick",
};
#define WP_PAT_COUNT 4

static void wp_draw(int wx, int wy, int ww, int wh)
{
    pw_draw_bg(wx, wy, ww, wh);
    int cx = wx + 16;
    int cy = wy + WM_TITLEBAR_H + 16;

    pw_label(cx, cy, "Workbench Pattern");
    cy += 30;

    pw_label(cx, cy, "Pattern:");
    g_wp_pattern.x = cx + 80; g_wp_pattern.y = cy - 2; g_wp_pattern.w = 200; g_wp_pattern.h = 20;
    g_wp_pattern.value = wp_patterns[g_wp_pat_idx];
    pw_cycle_draw(&g_wp_pattern);
    cy += 40;

    /* Preview box */
    pw_label(cx, cy, "Preview:");
    int px = cx + 80;
    int py = cy;
    int pw = 120;
    int ph = 60;
    FB_FillRect(px, py, pw, ph, WB_GREY);
    pw_bevel(px, py, pw, ph, 0);

    if (g_wp_pat_idx == 1) { /* Checkerboard */
        for (int yy = 0; yy < ph; yy += 8)
            for (int xx = 0; xx < pw; xx += 8)
                if (((xx / 8) + (yy / 8)) & 1)
                    FB_FillRect(px + xx, py + yy, 8, 8, WB_DARK_GREY);
    } else if (g_wp_pat_idx == 2) { /* Dots */
        for (int yy = 4; yy < ph; yy += 8)
            for (int xx = 4; xx < pw; xx += 8)
                FB_PutPixel(px + xx, py + yy, WB_DARK_GREY);
    } else if (g_wp_pat_idx == 3) { /* Brick */
        for (int yy = 0; yy < ph; yy += 8) {
            FB_DrawHLine(px, py + yy, pw, WB_DARK_GREY);
            int off = (yy / 8) & 1 ? 0 : 16;
            for (int xx = off; xx < pw; xx += 32)
                FB_DrawVLine(px + xx, py + yy, 8, WB_DARK_GREY);
        }
    }

    pw_btn_row(&g_wp_apply, &g_wp_save, &g_wp_close, wx, wy, ww, wh);
    pw_draw_btn_row(&g_wp_apply, &g_wp_save, &g_wp_close);
}

static void wp_key(char c)
{
    if (c == 27) { WM_CloseWindow(g_wp_handle); g_wp_handle = -1; }
}

static void wp_click(int handle, int mx, int my)
{
    (void)handle;
    if (pw_cycle_hit_up(&g_wp_pattern, mx, my)) { g_wp_pat_idx = (g_wp_pat_idx + 1) % WP_PAT_COUNT; WM_Redraw(); return; }
    if (pw_cycle_hit_dn(&g_wp_pattern, mx, my)) { g_wp_pat_idx = (g_wp_pat_idx - 1 + WP_PAT_COUNT) % WP_PAT_COUNT; WM_Redraw(); return; }
    if (pw_btn_hit(&g_wp_apply, mx, my)) { WM_Redraw(); return; }
    if (pw_btn_hit(&g_wp_save, mx, my)) { WM_Redraw(); return; }
    if (pw_btn_hit(&g_wp_close, mx, my)) { WM_CloseWindow(g_wp_handle); g_wp_handle = -1; return; }
}

void WBPatternPrefs_Show(void)
{
    if (g_wp_handle >= 0 && WM_GetDrawFn(g_wp_handle) == wp_draw) {
        WM_RaiseWindow(g_wp_handle); WM_Redraw(); return;
    }
    g_wp_handle = -1;
    int wx = ((int)g_fb.width - WP_WIN_W) / 2;
    int wy = ((int)g_fb.height - WP_WIN_H) / 2;
    if (wy < WM_TITLEBAR_H + 4) wy = WM_TITLEBAR_H + 4;
    g_wp_handle = WM_AddWindow(wx, wy, WP_WIN_W, WP_WIN_H, "WBPattern Prefs", wp_draw, wp_key);
    if (g_wp_handle >= 0) { WM_SetClickHandler(g_wp_handle, wp_click); WM_Redraw(); }
}

/* =========================================================================
 * Font Preferences
 * ========================================================================= */

#define FT_WIN_W  340
#define FT_WIN_H  220

static int g_ft_handle = -1;
static PwCycle g_ft_font;
static PwBtn g_ft_close;
static int g_ft_idx = 0;

static const char *ft_fonts[] = { "Topaz 8", "Topaz 9", "Courier 10" };
#define FT_COUNT 3

static void ft_draw(int wx, int wy, int ww, int wh)
{
    pw_draw_bg(wx, wy, ww, wh);
    int cx = wx + 16;
    int cy = wy + WM_TITLEBAR_H + 16;

    pw_label(cx, cy, "Font Preferences");
    cy += 30;

    pw_label(cx, cy, "System Font:");
    g_ft_font.x = cx + 100; g_ft_font.y = cy - 2; g_ft_font.w = 180; g_ft_font.h = 20;
    g_ft_font.value = ft_fonts[g_ft_idx];
    pw_cycle_draw(&g_ft_font);
    cy += 40;

    pw_label(cx, cy, "Preview:");
    FB_FillRect(cx + 80, cy, 200, 30, WB_WHITE);
    pw_bevel(cx + 80, cy, 200, 30, 0);
    FB_PutStr(cx + 88, cy + 7, "The quick brown fox", COL_LABEL, WB_WHITE);

    g_ft_close.x = wx + (ww - PW_BTN_W) / 2; g_ft_close.y = wy + wh - WM_TITLEBAR_H - 30;
    g_ft_close.w = PW_BTN_W; g_ft_close.h = PW_BTN_H; g_ft_close.label = "Close";
    pw_btn_draw(&g_ft_close, 0);
}

static void ft_key(char c)
{
    if (c == 27) { WM_CloseWindow(g_ft_handle); g_ft_handle = -1; }
}

static void ft_click(int handle, int mx, int my)
{
    (void)handle;
    if (pw_cycle_hit_up(&g_ft_font, mx, my)) { g_ft_idx = (g_ft_idx + 1) % FT_COUNT; WM_Redraw(); return; }
    if (pw_cycle_hit_dn(&g_ft_font, mx, my)) { g_ft_idx = (g_ft_idx - 1 + FT_COUNT) % FT_COUNT; WM_Redraw(); return; }
    if (pw_btn_hit(&g_ft_close, mx, my)) { WM_CloseWindow(g_ft_handle); g_ft_handle = -1; return; }
}

void FontPrefs_Show(void)
{
    if (g_ft_handle >= 0 && WM_GetDrawFn(g_ft_handle) == ft_draw) {
        WM_RaiseWindow(g_ft_handle); WM_Redraw(); return;
    }
    g_ft_handle = -1;
    int wx = ((int)g_fb.width - FT_WIN_W) / 2;
    int wy = ((int)g_fb.height - FT_WIN_H) / 2;
    if (wy < WM_TITLEBAR_H + 4) wy = WM_TITLEBAR_H + 4;
    g_ft_handle = WM_AddWindow(wx, wy, FT_WIN_W, FT_WIN_H, "Font Prefs", ft_draw, ft_key);
    if (g_ft_handle >= 0) { WM_SetClickHandler(g_ft_handle, ft_click); WM_Redraw(); }
}

/* =========================================================================
 * Serial Preferences
 * ========================================================================= */

#define SR_WIN_W  340
#define SR_WIN_H  240

static int g_sr_handle = -1;
static PwCycle g_sr_baud, g_sr_parity;
static PwBtn g_sr_close;
static int g_sr_baud_idx = 4; /* 9600 */
static int g_sr_parity_idx = 0; /* None */

static const char *sr_bauds[] = { "300", "1200", "2400", "4800", "9600", "19200", "38400", "57600" };
#define SR_BAUD_COUNT 8
static const char *sr_parities[] = { "None", "Even", "Odd" };
#define SR_PAR_COUNT 3

static void sr_draw(int wx, int wy, int ww, int wh)
{
    pw_draw_bg(wx, wy, ww, wh);
    int cx = wx + 16;
    int cy = wy + WM_TITLEBAR_H + 16;

    pw_label(cx, cy, "Serial Port Preferences");
    cy += 30;

    pw_label(cx, cy, "Baud Rate:");
    g_sr_baud.x = cx + 100; g_sr_baud.y = cy - 2; g_sr_baud.w = 140; g_sr_baud.h = 20;
    g_sr_baud.value = sr_bauds[g_sr_baud_idx];
    pw_cycle_draw(&g_sr_baud);
    cy += 30;

    pw_label(cx, cy, "Parity:");
    g_sr_parity.x = cx + 100; g_sr_parity.y = cy - 2; g_sr_parity.w = 140; g_sr_parity.h = 20;
    g_sr_parity.value = sr_parities[g_sr_parity_idx];
    pw_cycle_draw(&g_sr_parity);

    g_sr_close.x = wx + (ww - PW_BTN_W) / 2; g_sr_close.y = wy + wh - WM_TITLEBAR_H - 30;
    g_sr_close.w = PW_BTN_W; g_sr_close.h = PW_BTN_H; g_sr_close.label = "Close";
    pw_btn_draw(&g_sr_close, 0);
}

static void sr_key(char c)
{
    if (c == 27) { WM_CloseWindow(g_sr_handle); g_sr_handle = -1; }
}

static void sr_click(int handle, int mx, int my)
{
    (void)handle;
    if (pw_cycle_hit_up(&g_sr_baud, mx, my)) { g_sr_baud_idx = (g_sr_baud_idx + 1) % SR_BAUD_COUNT; WM_Redraw(); return; }
    if (pw_cycle_hit_dn(&g_sr_baud, mx, my)) { g_sr_baud_idx = (g_sr_baud_idx - 1 + SR_BAUD_COUNT) % SR_BAUD_COUNT; WM_Redraw(); return; }
    if (pw_cycle_hit_up(&g_sr_parity, mx, my)) { g_sr_parity_idx = (g_sr_parity_idx + 1) % SR_PAR_COUNT; WM_Redraw(); return; }
    if (pw_cycle_hit_dn(&g_sr_parity, mx, my)) { g_sr_parity_idx = (g_sr_parity_idx - 1 + SR_PAR_COUNT) % SR_PAR_COUNT; WM_Redraw(); return; }
    if (pw_btn_hit(&g_sr_close, mx, my)) { WM_CloseWindow(g_sr_handle); g_sr_handle = -1; return; }
}

void SerialPrefs_Show(void)
{
    if (g_sr_handle >= 0 && WM_GetDrawFn(g_sr_handle) == sr_draw) {
        WM_RaiseWindow(g_sr_handle); WM_Redraw(); return;
    }
    g_sr_handle = -1;
    int wx = ((int)g_fb.width - SR_WIN_W) / 2;
    int wy = ((int)g_fb.height - SR_WIN_H) / 2;
    if (wy < WM_TITLEBAR_H + 4) wy = WM_TITLEBAR_H + 4;
    g_sr_handle = WM_AddWindow(wx, wy, SR_WIN_W, SR_WIN_H, "Serial Prefs", sr_draw, sr_key);
    if (g_sr_handle >= 0) { WM_SetClickHandler(g_sr_handle, sr_click); WM_Redraw(); }
}

/* =========================================================================
 * Printer Preferences
 * ========================================================================= */

#define PR_WIN_W  340
#define PR_WIN_H  240

static int g_pr_handle = -1;
static PwCycle g_pr_type, g_pr_port;
static PwBtn g_pr_close;
static int g_pr_type_idx = 0;
static int g_pr_port_idx = 0;

static const char *pr_types[] = { "Generic", "PostScript", "HP DeskJet", "Epson" };
#define PR_TYPE_COUNT 4
static const char *pr_ports[] = { "Parallel", "Serial", "USB" };
#define PR_PORT_COUNT 3

static void pr_draw(int wx, int wy, int ww, int wh)
{
    pw_draw_bg(wx, wy, ww, wh);
    int cx = wx + 16;
    int cy = wy + WM_TITLEBAR_H + 16;

    pw_label(cx, cy, "Printer Preferences");
    cy += 30;

    pw_label(cx, cy, "Printer Type:");
    g_pr_type.x = cx + 110; g_pr_type.y = cy - 2; g_pr_type.w = 160; g_pr_type.h = 20;
    g_pr_type.value = pr_types[g_pr_type_idx];
    pw_cycle_draw(&g_pr_type);
    cy += 30;

    pw_label(cx, cy, "Port:");
    g_pr_port.x = cx + 110; g_pr_port.y = cy - 2; g_pr_port.w = 160; g_pr_port.h = 20;
    g_pr_port.value = pr_ports[g_pr_port_idx];
    pw_cycle_draw(&g_pr_port);

    g_pr_close.x = wx + (ww - PW_BTN_W) / 2; g_pr_close.y = wy + wh - WM_TITLEBAR_H - 30;
    g_pr_close.w = PW_BTN_W; g_pr_close.h = PW_BTN_H; g_pr_close.label = "Close";
    pw_btn_draw(&g_pr_close, 0);
}

static void pr_key(char c)
{
    if (c == 27) { WM_CloseWindow(g_pr_handle); g_pr_handle = -1; }
}

static void pr_click(int handle, int mx, int my)
{
    (void)handle;
    if (pw_cycle_hit_up(&g_pr_type, mx, my)) { g_pr_type_idx = (g_pr_type_idx + 1) % PR_TYPE_COUNT; WM_Redraw(); return; }
    if (pw_cycle_hit_dn(&g_pr_type, mx, my)) { g_pr_type_idx = (g_pr_type_idx - 1 + PR_TYPE_COUNT) % PR_TYPE_COUNT; WM_Redraw(); return; }
    if (pw_cycle_hit_up(&g_pr_port, mx, my)) { g_pr_port_idx = (g_pr_port_idx + 1) % PR_PORT_COUNT; WM_Redraw(); return; }
    if (pw_cycle_hit_dn(&g_pr_port, mx, my)) { g_pr_port_idx = (g_pr_port_idx - 1 + PR_PORT_COUNT) % PR_PORT_COUNT; WM_Redraw(); return; }
    if (pw_btn_hit(&g_pr_close, mx, my)) { WM_CloseWindow(g_pr_handle); g_pr_handle = -1; return; }
}

void PrinterPrefs_Show(void)
{
    if (g_pr_handle >= 0 && WM_GetDrawFn(g_pr_handle) == pr_draw) {
        WM_RaiseWindow(g_pr_handle); WM_Redraw(); return;
    }
    g_pr_handle = -1;
    int wx = ((int)g_fb.width - PR_WIN_W) / 2;
    int wy = ((int)g_fb.height - PR_WIN_H) / 2;
    if (wy < WM_TITLEBAR_H + 4) wy = WM_TITLEBAR_H + 4;
    g_pr_handle = WM_AddWindow(wx, wy, PR_WIN_W, PR_WIN_H, "Printer Prefs", pr_draw, pr_key);
    if (g_pr_handle >= 0) { WM_SetClickHandler(g_pr_handle, pr_click); WM_Redraw(); }
}

/* =========================================================================
 * Locale Preferences
 * ========================================================================= */

#define LO_WIN_W  340
#define LO_WIN_H  240

static int g_lo_handle = -1;
static PwCycle g_lo_lang, g_lo_country;
static PwBtn g_lo_close;
static int g_lo_lang_idx = 0;
static int g_lo_country_idx = 0;

static const char *lo_langs[] = { "English", "Deutsch", "Francais", "Italiano" };
#define LO_LANG_COUNT 4
static const char *lo_countries[] = { "USA", "UK", "Deutschland", "France" };
#define LO_COUNTRY_COUNT 4

static void lo_draw(int wx, int wy, int ww, int wh)
{
    pw_draw_bg(wx, wy, ww, wh);
    int cx = wx + 16;
    int cy = wy + WM_TITLEBAR_H + 16;

    pw_label(cx, cy, "Locale Preferences");
    cy += 30;

    pw_label(cx, cy, "Language:");
    g_lo_lang.x = cx + 100; g_lo_lang.y = cy - 2; g_lo_lang.w = 160; g_lo_lang.h = 20;
    g_lo_lang.value = lo_langs[g_lo_lang_idx];
    pw_cycle_draw(&g_lo_lang);
    cy += 30;

    pw_label(cx, cy, "Country:");
    g_lo_country.x = cx + 100; g_lo_country.y = cy - 2; g_lo_country.w = 160; g_lo_country.h = 20;
    g_lo_country.value = lo_countries[g_lo_country_idx];
    pw_cycle_draw(&g_lo_country);

    g_lo_close.x = wx + (ww - PW_BTN_W) / 2; g_lo_close.y = wy + wh - WM_TITLEBAR_H - 30;
    g_lo_close.w = PW_BTN_W; g_lo_close.h = PW_BTN_H; g_lo_close.label = "Close";
    pw_btn_draw(&g_lo_close, 0);
}

static void lo_key(char c)
{
    if (c == 27) { WM_CloseWindow(g_lo_handle); g_lo_handle = -1; }
}

static void lo_click(int handle, int mx, int my)
{
    (void)handle;
    if (pw_cycle_hit_up(&g_lo_lang, mx, my)) { g_lo_lang_idx = (g_lo_lang_idx + 1) % LO_LANG_COUNT; WM_Redraw(); return; }
    if (pw_cycle_hit_dn(&g_lo_lang, mx, my)) { g_lo_lang_idx = (g_lo_lang_idx - 1 + LO_LANG_COUNT) % LO_LANG_COUNT; WM_Redraw(); return; }
    if (pw_cycle_hit_up(&g_lo_country, mx, my)) { g_lo_country_idx = (g_lo_country_idx + 1) % LO_COUNTRY_COUNT; WM_Redraw(); return; }
    if (pw_cycle_hit_dn(&g_lo_country, mx, my)) { g_lo_country_idx = (g_lo_country_idx - 1 + LO_COUNTRY_COUNT) % LO_COUNTRY_COUNT; WM_Redraw(); return; }
    if (pw_btn_hit(&g_lo_close, mx, my)) { WM_CloseWindow(g_lo_handle); g_lo_handle = -1; return; }
}

void LocalePrefs_Show(void)
{
    if (g_lo_handle >= 0 && WM_GetDrawFn(g_lo_handle) == lo_draw) {
        WM_RaiseWindow(g_lo_handle); WM_Redraw(); return;
    }
    g_lo_handle = -1;
    int wx = ((int)g_fb.width - LO_WIN_W) / 2;
    int wy = ((int)g_fb.height - LO_WIN_H) / 2;
    if (wy < WM_TITLEBAR_H + 4) wy = WM_TITLEBAR_H + 4;
    g_lo_handle = WM_AddWindow(wx, wy, LO_WIN_W, LO_WIN_H, "Locale Prefs", lo_draw, lo_key);
    if (g_lo_handle >= 0) { WM_SetClickHandler(g_lo_handle, lo_click); WM_Redraw(); }
}

/* =========================================================================
 * Any-open check
 * ========================================================================= */

int PrefsWin_AnyOpen(void)
{
    return (g_pal_handle >= 0 || g_time_handle >= 0 ||
            g_ic_handle >= 0 || g_in_handle >= 0 ||
            g_sm_handle >= 0 || g_wp_handle >= 0 ||
            g_ft_handle >= 0 || g_sr_handle >= 0 ||
            g_pr_handle >= 0 || g_lo_handle >= 0);
}
