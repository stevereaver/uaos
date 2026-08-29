/* exchange_win.c — UAOS Commodities Exchange
 *
 * Lists all registered commodity brokers with status indicators
 * and provides Enable/Disable/Sleep/Wake/Cycle controls.
 */

#include "exchange_win.h"
#include "commodities.h"
#include "framebuffer.h"
#include "wm.h"
#include <stdint.h>

#define EX_WIN_W   440
#define EX_WIN_H   320
#define EX_ROW_H   22
#define EX_LIST_X  12
#define EX_LIST_Y  (WM_TITLEBAR_H + 30)
#define EX_LIST_W  (EX_WIN_W - 24)
#define EX_BTN_W   72
#define EX_BTN_H   20

static int g_ex_handle = -1;
static int g_ex_sel    = 0;  /* selected broker index in list */

/* Button gadgets */
typedef struct { int x, y, w, h; const char *label; } ExBtn;

static ExBtn g_ex_cycle, g_ex_sleep, g_ex_wake, g_ex_disable, g_ex_close;

/* --- helpers --- */

static int ex_slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void ex_bevel(int x, int y, int w, int h, int raised)
{
    uint32_t hi = raised ? WB_WHITE : WB_DARK_GREY;
    uint32_t lo = raised ? WB_DARK_GREY : WB_WHITE;
    FB_DrawHLine(x, y, w, hi);
    FB_DrawVLine(x, y, h, hi);
    FB_DrawHLine(x, y + h - 1, w, lo);
    FB_DrawVLine(x + w - 1, y, h, lo);
}

static void ex_btn_draw(ExBtn *b, int pressed)
{
    uint32_t bg = pressed ? WB_DARK_GREY : WB_LIGHT_GREY;
    FB_FillRect(b->x, b->y, b->w, b->h, bg);
    ex_bevel(b->x, b->y, b->w, b->h, !pressed);
    int tw = ex_slen(b->label) * 8;
    int tx = b->x + (b->w - tw) / 2;
    int ty = b->y + (b->h - 16) / 2;
    FB_PutStr(tx, ty, b->label, WB_BLACK, bg);
}

static int ex_btn_hit(ExBtn *b, int mx, int my)
{
    return (mx >= b->x && mx < b->x + b->w &&
            my >= b->y && my < b->y + b->h);
}

static const char *state_str(CxState s)
{
    switch (s) {
        case CX_STATE_ACTIVE:   return "Active";
        case CX_STATE_SLEEPING: return "Sleep";
        case CX_STATE_DISABLED: return "Off";
    }
    return "?";
}

static uint32_t state_color(CxState s)
{
    switch (s) {
        case CX_STATE_ACTIVE:   return WB_BLUE;
        case CX_STATE_SLEEPING: return WB_ORANGE;
        case CX_STATE_DISABLED: return WB_DARK_GREY;
    }
    return WB_DARK_GREY;
}

/* --- draw --- */

static void ex_draw(int wx, int wy, int ww, int wh)
{
    /* Background */
    FB_FillRect(wx, wy + WM_TITLEBAR_H, ww, wh - WM_TITLEBAR_H, WB_GREY);

    /* Header */
    int hx = wx + 12;
    int hy = wy + WM_TITLEBAR_H + 8;
    FB_PutStr(hx, hy, "Commodity Brokers:", WB_BLACK, WB_GREY);

    /* Column headers */
    int ly = wy + EX_LIST_Y;
    FB_PutStr(wx + EX_LIST_X, ly, "Name", WB_WHITE, WB_BLUE);
    FB_PutStr(wx + EX_LIST_X + 120, ly, "Status", WB_WHITE, WB_BLUE);
    FB_PutStr(wx + EX_LIST_X + 180, ly, "Description", WB_WHITE, WB_BLUE);
    FB_FillRect(wx + EX_LIST_X + ex_slen("Description") * 8 + 180,
                ly, EX_LIST_W - ex_slen("Description") * 8 - 180, 16, WB_BLUE);

    ly += 18;

    /* Broker rows */
    int row = 0;
    for (int i = 0; i < CX_MAX_BROKERS; i++) {
        const CxBroker *b = Cx_GetBroker(i);
        if (!b) continue;

        int ry = ly + row * EX_ROW_H;
        uint32_t bg = (i == g_ex_sel) ? WB_BLUE : WB_GREY;
        uint32_t fg = (i == g_ex_sel) ? WB_WHITE : WB_BLACK;

        FB_FillRect(wx + EX_LIST_X, ry, EX_LIST_W, EX_ROW_H - 2, bg);

        /* Name */
        FB_PutStr(wx + EX_LIST_X + 4, ry + 3, b->name, fg, bg);

        /* Status indicator (colored dot + text) */
        uint32_t sc = state_color(b->state);
        FB_FillRect(wx + EX_LIST_X + 120, ry + 6, 10, 10, sc);
        ex_bevel(wx + EX_LIST_X + 120, ry + 6, 10, 10, 1);
        FB_PutStr(wx + EX_LIST_X + 134, ry + 3, state_str(b->state), fg, bg);

        /* Description */
        FB_PutStr(wx + EX_LIST_X + 180, ry + 3, b->desc, fg, bg);

        row++;
    }

    /* Buttons at bottom */
    int by = wy + wh - WM_TITLEBAR_H - EX_BTN_H - 8;
    int bx = wx + 12;
    g_ex_cycle.x = bx;   g_ex_cycle.y = by;   g_ex_cycle.w = EX_BTN_W; g_ex_cycle.h = EX_BTN_H;
    g_ex_cycle.label = "Cycle";
    bx += EX_BTN_W + 8;
    g_ex_sleep.x = bx;   g_ex_sleep.y = by;   g_ex_sleep.w = EX_BTN_W; g_ex_sleep.h = EX_BTN_H;
    g_ex_sleep.label = "Sleep";
    bx += EX_BTN_W + 8;
    g_ex_wake.x = bx;    g_ex_wake.y = by;    g_ex_wake.w = EX_BTN_W; g_ex_wake.h = EX_BTN_H;
    g_ex_wake.label = "Wake";
    bx += EX_BTN_W + 8;
    g_ex_disable.x = bx; g_ex_disable.y = by; g_ex_disable.w = EX_BTN_W; g_ex_disable.h = EX_BTN_H;
    g_ex_disable.label = "Disable";
    bx += EX_BTN_W + 8;
    g_ex_close.x = bx;   g_ex_close.y = by;   g_ex_close.w = EX_BTN_W; g_ex_close.h = EX_BTN_H;
    g_ex_close.label = "Close";

    ex_btn_draw(&g_ex_cycle, 0);
    ex_btn_draw(&g_ex_sleep, 0);
    ex_btn_draw(&g_ex_wake, 0);
    ex_btn_draw(&g_ex_disable, 0);
    ex_btn_draw(&g_ex_close, 0);
}

/* --- input --- */

static void ex_key(char c)
{
    if (c == 27) { /* ESC */
        WM_CloseWindow(g_ex_handle);
        g_ex_handle = -1;
    }
}

static int ex_broker_at_row(int row)
{
    int count = 0;
    for (int i = 0; i < CX_MAX_BROKERS; i++) {
        if (!Cx_GetBroker(i)) continue;
        if (count == row) return i;
        count++;
    }
    return -1;
}

static void ex_click(int handle, int mx, int my)
{
    (void)handle;

    /* List row click — select */
    int wwx, wwy, www, wwh;
    WM_GetWindowRect(g_ex_handle, &wwx, &wwy, &www, &wwh);
    int ly = wwy + EX_LIST_Y + 18;
    int lx = wwx + EX_LIST_X;
    int rw = EX_LIST_W;

    if (mx >= lx && mx < lx + rw && my >= ly) {
        int row = (my - ly) / EX_ROW_H;
        int idx = ex_broker_at_row(row);
        if (idx >= 0) {
            g_ex_sel = idx;
            WM_Redraw();
            return;
        }
    }

    /* Button clicks */
    if (ex_btn_hit(&g_ex_cycle, mx, my)) {
        Cx_CycleState(g_ex_sel);
        WM_Redraw();
        return;
    }
    if (ex_btn_hit(&g_ex_sleep, mx, my)) {
        Cx_Sleep(g_ex_sel);
        WM_Redraw();
        return;
    }
    if (ex_btn_hit(&g_ex_wake, mx, my)) {
        Cx_Wake(g_ex_sel);
        WM_Redraw();
        return;
    }
    if (ex_btn_hit(&g_ex_disable, mx, my)) {
        Cx_Disable(g_ex_sel);
        WM_Redraw();
        return;
    }
    if (ex_btn_hit(&g_ex_close, mx, my)) {
        WM_CloseWindow(g_ex_handle);
        g_ex_handle = -1;
        return;
    }
}

/* --- public --- */

void ExchangeWin_Show(void)
{
    if (g_ex_handle >= 0 && WM_GetDrawFn(g_ex_handle) == ex_draw) {
        WM_RaiseWindow(g_ex_handle);
        WM_Redraw();
        return;
    }
    g_ex_handle = -1;

    int wx = ((int)g_fb.width - EX_WIN_W) / 2;
    int wy = ((int)g_fb.height - EX_WIN_H) / 2;
    if (wy < WM_TITLEBAR_H + 4) wy = WM_TITLEBAR_H + 4;

    g_ex_handle = WM_AddWindow(wx, wy, EX_WIN_W, EX_WIN_H, "Exchange", ex_draw, ex_key);
    if (g_ex_handle >= 0) {
        WM_SetClickHandler(g_ex_handle, ex_click);
        WM_Redraw();
    }
}

void ExchangeWin_Refresh(void)
{
    if (g_ex_handle >= 0)
        WM_Redraw();
}
