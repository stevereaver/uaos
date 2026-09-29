/* gadgets.c — UAOS shared kernel gadget set
 *
 * Rendering ported from prefs_win.c (Pw* widgets), requester.c and
 * format_win.c so all tools share one look and one input path.
 * WB 3.1 metrics: ~22px gadgets, 14px checkboxes, 8x16 text grid.
 */

#include "gadgets.h"
#include "framebuffer.h"
#include "wm.h"
#include "../irq/ps2kbd.h"
#include <stddef.h>

/* Palette (matches the old prefs_win.c COL_* set) */
#define GC_BG        WB_GREY
#define GC_LABEL     WB_BLACK
#define GC_BORDER    WB_DARK_GREY
#define GC_BTN       WB_LIGHT_GREY
#define GC_BTN_PRS   WB_DARK_GREY
#define GC_SEL       WB_BLUE
#define GC_SEL_FG    WB_WHITE
#define GC_FIELD     WB_WHITE
#define GC_FIELD_OFF WB_LIGHT_GREY

/* =========================================================================
 * Small text helpers (kernel — no libc reliance beyond basics)
 * ========================================================================= */

int gad_slen(const char *s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

void gad_str_cp(char *dst, const char *src, int max)
{
    int i = 0;
    if (!dst || max <= 0) return;
    if (src)
        while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

int gad_str_eq(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

void gad_itoa(char *buf, int val)
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
 * Shared drawing helpers
 * ========================================================================= */

void gad_bevel(int x, int y, int w, int h, int raised)
{
    uint32_t hi = raised ? WB_WHITE     : WB_DARK_GREY;
    uint32_t lo = raised ? WB_DARK_GREY : WB_WHITE;
    FB_DrawHLine(x,         y,         w, hi);
    FB_DrawVLine(x,         y,         h, hi);
    FB_DrawHLine(x,         y + h - 1, w, lo);
    FB_DrawVLine(x + w - 1, y,         h, lo);
}

void gad_label(int x, int y, const char *s)
{
    if (s) FB_PutStr(x, y, s, GC_LABEL, GC_BG);
}

void gad_gbox(int x, int y, int w, int h, const char *title)
{
    gad_bevel(x, y, w, h, 0);
    if (title && title[0]) {
        int tw = gad_slen(title) * 8;
        FB_FillRect(x + 8, y - 1, tw + 8, 4, GC_BG);   /* punch frame line */
        FB_PutStr(x + 12, y - 8, title, GC_LABEL, GC_BG);
    }
}

void gad_bg(int wx, int wy, int w, int h)
{
    FB_FillRect(wx, wy + WM_TITLEBAR_H, w, h - WM_TITLEBAR_H, GC_BG);
}

void gad_btn_row(Gad *apply, Gad *save, Gad *close,
                 int wx, int wy, int ww, int wh)
{
    int y = wy + wh - WM_TITLEBAR_H - GAD_BTN_H - 8;
    int total = GAD_BTN_W * 3 + GAD_BTN_GAP * 2;
    int x = wx + (ww - total) / 2;

    Gad *row[3] = { apply, save, close };
    static const char *names[3] = { "Apply", "Save", "Close" };
    for (int i = 0; i < 3; i++) {
        if (!row[i]) continue;
        row[i]->kind = GAD_BUTTON;
        row[i]->x = x; row[i]->y = y;
        row[i]->w = GAD_BTN_W; row[i]->h = GAD_BTN_H;
        row[i]->text = names[i];
        x += GAD_BTN_W + GAD_BTN_GAP;
    }
}

/* =========================================================================
 * Per-kind drawing
 * ========================================================================= */

static void draw_button(const Gad *g)
{
    uint32_t bg = (g->pressed && !(g->flags & GADF_DISABLED))
                  ? GC_BTN_PRS : GC_BTN;
    FB_FillRect(g->x, g->y, g->w, g->h, bg);
    gad_bevel(g->x, g->y, g->w, g->h, !(g->pressed));
    if (g->text) {
        int tw = gad_slen(g->text) * 8;
        FB_PutStr(g->x + (g->w - tw) / 2,
                  g->y + (g->h - 16) / 2,
                  g->text, GC_LABEL, bg);
    }
}

static void draw_checkbox(const Gad *g)
{
    FB_FillRect(g->x, g->y, GAD_CHECK_SZ, GAD_CHECK_SZ, GC_FIELD);
    gad_bevel(g->x, g->y, GAD_CHECK_SZ, GAD_CHECK_SZ, 0);
    if (g->val)
        FB_PutStr(g->x + 2, g->y - 1, "X", GC_LABEL, GC_FIELD);
    if (g->text)
        FB_PutStr(g->x + GAD_LABEL_DX, g->y - 1, g->text, GC_LABEL, GC_BG);
}

/* 10px-wide scanline disc for the radio indicator. */
static void draw_radio(const Gad *g)
{
    static const uint8_t rw[10] = {4, 8, 10, 10, 10, 10, 10, 10, 8, 4};
    for (int i = 0; i < 10; i++) {
        int w  = rw[i];
        int x0 = g->x + 2 + (10 - w) / 2;
        int yy = g->y + 2 + i;
        FB_FillRect(x0, yy, w, 1, GC_FIELD);
        FB_PutPixel(x0,         yy, GC_BORDER);
        FB_PutPixel(x0 + w - 1, yy, GC_BORDER);
    }
    if (g->val)
        FB_FillRect(g->x + 5, g->y + 5, 4, 4, GC_LABEL);
    if (g->text)
        FB_PutStr(g->x + GAD_LABEL_DX, g->y - 1, g->text, GC_LABEL, GC_BG);
}

static void draw_cycle(const Gad *g)
{
    FB_FillRect(g->x, g->y, g->w, g->h, GC_BTN);
    gad_bevel(g->x, g->y, g->w, g->h, 1);
    const char *v = (g->choices && g->val >= 0 && g->val < g->nchoices)
                    ? g->choices[g->val] : (g->text ? g->text : "");
    FB_PutStr(g->x + 4, g->y + (g->h - 16) / 2, v, GC_LABEL, GC_BTN);
    int ax = g->x + g->w - GAD_ARROW_W - 2;
    int ah = g->h / 2;
    FB_FillRect(ax, g->y, GAD_ARROW_W, ah, GC_BTN);
    gad_bevel(ax, g->y, GAD_ARROW_W, ah, 1);
    FB_PutStr(ax + 4, g->y + (ah - 16) / 2, "^", GC_LABEL, GC_BTN);
    FB_FillRect(ax, g->y + ah, GAD_ARROW_W, ah, GC_BTN);
    gad_bevel(ax, g->y + ah, GAD_ARROW_W, ah, 1);
    FB_PutStr(ax + 4, g->y + ah + (ah - 16) / 2, "v", GC_LABEL, GC_BTN);
}

static void draw_slider(const Gad *g)
{
    int vert   = (g->flags & GADF_VERTICAL) != 0;
    int len    = vert ? g->h : g->w;
    int range  = g->max - g->min;
    int kpos   = (range <= 0) ? 0 : ((g->val - g->min) * (len - 16)) / range;

    if (vert) {
        int tx = g->x + (g->w - 6) / 2;
        FB_FillRect(tx, g->y, 6, g->h, GC_FIELD);
        gad_bevel(tx, g->y, 6, g->h, 0);
        FB_FillRect(g->x, g->y + kpos, g->w, 16, GC_BTN);
        gad_bevel(g->x, g->y + kpos, g->w, 16, 1);
    } else {
        int ty = g->y + (g->h - 6) / 2;
        FB_FillRect(g->x, ty, g->w, 6, GC_FIELD);
        gad_bevel(g->x, ty, g->w, 6, 0);
        FB_FillRect(g->x + kpos, g->y, 16, g->h, GC_BTN);
        gad_bevel(g->x + kpos, g->y, 16, g->h, 1);
    }
}

static void draw_string(const Gad *g)
{
    uint32_t bg = g->focused ? GC_FIELD : GC_FIELD_OFF;
    FB_FillRect(g->x, g->y, g->w, g->h, bg);
    gad_bevel(g->x, g->y, g->w, g->h, 0);
    if (g->buf)
        FB_PutStr(g->x + 4, g->y + 3, g->buf, GC_LABEL, bg);
    if (g->focused) {
        int cx = g->x + 4 + g->cursor * 8;
        int mx = g->x + g->w - 3;
        if (cx > mx) cx = mx;
        FB_DrawVLine(cx, g->y + 3, g->h - 6, GC_LABEL);
    }
}

static void draw_listview(const Gad *g)
{
    FB_FillRect(g->x, g->y, g->w, g->h, GC_FIELD);
    gad_bevel(g->x, g->y, g->w, g->h, 0);
    int rows = (g->h - 4) / GAD_ROW_H;
    for (int i = 0; i < rows && i < g->nchoices; i++) {
        int ry = g->y + 2 + i * GAD_ROW_H;
        if (i == g->sel) {
            FB_FillRect(g->x + 1, ry, g->w - 2, GAD_ROW_H, GC_SEL);
            FB_PutStr(g->x + 4, ry + 1, g->choices[i], GC_SEL_FG, GC_SEL);
        } else {
            FB_PutStr(g->x + 4, ry + 1, g->choices[i], GC_LABEL, GC_FIELD);
        }
    }
}

void gad_draw(const Gad *g)
{
    if (!g) return;
    switch (g->kind) {
        case GAD_BUTTON:   draw_button(g);   break;
        case GAD_CHECKBOX: draw_checkbox(g); break;
        case GAD_RADIO:    draw_radio(g);    break;
        case GAD_CYCLE:    draw_cycle(g);    break;
        case GAD_SLIDER:   draw_slider(g);   break;
        case GAD_STRING:
        case GAD_INTEGER:  draw_string(g);   break;
        case GAD_LISTVIEW: draw_listview(g); break;
        case GAD_GBOX:     gad_gbox(g->x, g->y, g->w, g->h, g->text); break;
        case GAD_LABEL:
        default:
            if (g->text) gad_label(g->x, g->y, g->text);
            break;
    }
}

/* =========================================================================
 * Hit testing
 * ========================================================================= */

int gad_hit(const Gad *g, int mx, int my)
{
    if (!g) return 0;
    switch (g->kind) {
        case GAD_CHECKBOX:
        case GAD_RADIO:
            return (mx >= g->x && my >= g->y && my < g->y + GAD_CHECK_SZ &&
                    mx < g->x + GAD_CHECK_SZ + GAD_LABEL_DX - 6 +
                         gad_slen(g->text) * 8);
        case GAD_LABEL:
        case GAD_GBOX:
            return 0;
        default:
            return (mx >= g->x && mx < g->x + g->w &&
                    my >= g->y && my < g->y + g->h);
    }
}

/* =========================================================================
 * Event dispatch
 * ========================================================================= */

void gad_slider_from_mouse(Gad *g, int m)
{
    int len   = (g->flags & GADF_VERTICAL) ? g->h : g->w;
    int base  = (g->flags & GADF_VERTICAL) ? g->y : g->x;
    int range = g->max - g->min;
    if (range <= 0 || len <= 16) { g->val = g->min; return; }
    int rel = m - base - 8;
    if (rel < 0) rel = 0;
    if (rel > len - 16) rel = len - 16;
    g->val = g->min + (rel * range) / (len - 16);
}

/* Cycle arrow regions: upper half of the arrow column = next,
 * lower half = previous.  A click on the value body cycles forward
 * (AmigaOS cycle gadgets advance on any click). */
static int cycle_prev_region(const Gad *g, int mx, int my)
{
    int ax = g->x + g->w - GAD_ARROW_W - 2;
    return (mx >= ax && my >= g->y + g->h / 2);
}

static int string_key(Gad *g, char c)
{
    if (!g->focused || !g->buf) return GADE_NONE;

    if (c == '\b') {
        if (g->cursor > 0) {
            for (int i = g->cursor - 1; i < g->buf_len; i++)
                g->buf[i] = g->buf[i + 1];
            g->cursor--; g->buf_len--;
        }
        return GADE_CHANGE;
    }
    if (c == KBD_VKEY_LEFT)  { if (g->cursor > 0) g->cursor--; return GADE_CHANGE; }
    if (c == KBD_VKEY_RIGHT) { if (g->cursor < g->buf_len) g->cursor++; return GADE_CHANGE; }
    if (c == KBD_VKEY_UP || c == KBD_VKEY_HOME)
        { g->cursor = 0;          return GADE_CHANGE; }
    if (c == KBD_VKEY_DOWN || c == KBD_VKEY_END)
        { g->cursor = g->buf_len; return GADE_CHANGE; }
    if (c == KBD_VKEY_DEL) {
        if (g->cursor < g->buf_len) {
            for (int i = g->cursor; i < g->buf_len; i++)
                g->buf[i] = g->buf[i + 1];
            g->buf_len--;
        }
        return GADE_CHANGE;
    }
    if (c >= 32 && c < 127) {
        if (g->kind == GAD_INTEGER && (c < '0' || c > '9'))
            return GADE_NONE;
        if (g->buf_len < g->buf_max - 1) {
            for (int i = g->buf_len; i >= g->cursor; i--)
                g->buf[i + 1] = g->buf[i];
            g->buf[g->cursor] = c;
            g->cursor++; g->buf_len++;
            return GADE_CHANGE;
        }
    }
    return GADE_NONE;
}

int gad_event(Gad *g, int phase, int a, int b)
{
    if (!g || (g->flags & GADF_DISABLED)) return GADE_NONE;

    if (phase == GAD_KEY) {
        if (g->kind == GAD_STRING || g->kind == GAD_INTEGER)
            return string_key(g, (char)a);
        return GADE_NONE;
    }

    int mx = a, my = b;
    int inside = gad_hit(g, mx, my);

    switch (g->kind) {
        case GAD_BUTTON:
            /* Kernel tools act on mouse-down (matches the old PwBtn
             * behaviour); pressed visuals are caller-managed via
             * g->pressed for tools that track press/release. */
            return (phase == GAD_DOWN && inside) ? GADE_CLICK : GADE_NONE;

        case GAD_CHECKBOX:
            if (phase == GAD_DOWN && inside) { g->val ^= 1; return GADE_CHANGE; }
            return GADE_NONE;

        case GAD_RADIO:
            if (phase == GAD_DOWN && inside && !g->val) {
                g->val = 1;
                return GADE_CHANGE;
            }
            return GADE_NONE;

        case GAD_CYCLE:
            if (phase == GAD_DOWN && inside && g->nchoices > 0) {
                if (cycle_prev_region(g, mx, my))
                    g->val = (g->val - 1 + g->nchoices) % g->nchoices;
                else
                    g->val = (g->val + 1) % g->nchoices;
                return GADE_CHANGE;
            }
            return GADE_NONE;

        case GAD_SLIDER:
            if ((phase == GAD_DOWN || phase == GAD_MOVE) && inside) {
                gad_slider_from_mouse(g,
                    (g->flags & GADF_VERTICAL) ? my : mx);
                return GADE_CHANGE;
            }
            return GADE_NONE;

        case GAD_STRING:
        case GAD_INTEGER:
            if (phase == GAD_DOWN) {
                if (inside) {
                    g->focused = 1;
                    int rel = (mx - g->x - 4) / 8;
                    if (rel < 0) rel = 0;
                    if (rel > g->buf_len) rel = g->buf_len;
                    g->cursor = rel;
                    return GADE_CHANGE;
                }
                if (g->focused) { g->focused = 0; return GADE_CHANGE; }
            }
            return GADE_NONE;

        case GAD_LISTVIEW:
            if (phase == GAD_DOWN && inside) {
                int row = (my - g->y - 2) / GAD_ROW_H;
                if (row >= 0 && row < g->nchoices && row != g->sel) {
                    g->sel = row;
                    return GADE_CHANGE;
                }
                return inside ? GADE_CLICK : GADE_NONE;
            }
            return GADE_NONE;

        default:
            return GADE_NONE;
    }
}
