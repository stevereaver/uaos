/* wm.c — UAOS Window Manager */

#include "wm.h"
#include "framebuffer.h"
#include "cursor.h"
#include "desktop.h"
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Window registry and z-order
 * ========================================================================= */

static WmWindow g_wins[WM_MAX_WINDOWS];
static int      g_zorder[WM_MAX_WINDOWS];  /* indices into g_wins, [0]=back */
static int      g_nwins = 0;

static int g_focus    = -1;   /* index into g_wins of focused window   */

/* Drag/resize state */
static int g_drag_handle  = -1;  /* window being dragged             */
static int g_drag_off_x   = 0;   /* cursor offset from window origin */
static int g_drag_off_y   = 0;
static int g_resize_handle = -1; /* window being resized             */
static int g_resize_base_w = 0;
static int g_resize_base_h = 0;
static int g_resize_orig_mx = 0;
static int g_resize_orig_my = 0;
static int g_btn_prev     = 0;   /* previous left-button state       */

#define WM_RESIZE_GRIP  16       /* size of bottom-right resize grip */

/* =========================================================================
 * Helpers
 * ========================================================================= */

static void str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Erase a window's footprint by repainting the desktop backdrop beneath it */
static void erase_window(int old_x, int old_y, int old_w, int old_h)
{
    Desktop_RedrawRect(old_x, old_y, old_w + 2, old_h + 2);
}

/* Draw a single window chrome (title bar + border) at current position */
static void draw_chrome(int wh)
{
    WmWindow *w = &g_wins[wh];
    int focused = (wh == g_focus);

    /* Outer border */
    FB_DrawRect(w->x, w->y, w->w, w->h, WB_DARK_GREY);

    /* Title bar fill */
    uint32_t tbar_col = focused ? WB_LIGHT_BLUE : WB_BLUE;
    FB_FillRect(w->x + 1, w->y + 1, w->w - 2, WM_TITLEBAR_H - 2, tbar_col);

    /* Title text centred in bar */
    FB_PutStrCentred(w->x + 1, w->y + 1, w->w - 2, WM_TITLEBAR_H - 2,
                     w->title, WB_WHITE, tbar_col);

    /* Close gadget — small box on left of title bar */
    FB_DrawRect(w->x + 3, w->y + 3, 14, 14, WB_WHITE);
    FB_FillRect(w->x + 4, w->y + 4, 12, 12, tbar_col);

    /* Window body background */
    FB_FillRect(w->x + 1, w->y + WM_TITLEBAR_H,
                w->w - 2, w->h - WM_TITLEBAR_H - 1, WB_GREY);

    /* Resize grip — hatched triangle in bottom-right corner */
    int gx = w->x + w->w - WM_RESIZE_GRIP - 1;
    int gy = w->y + w->h - WM_RESIZE_GRIP - 1;
    for (int i = 2; i < WM_RESIZE_GRIP; i += 4) {
        FB_DrawHLine(gx + (WM_RESIZE_GRIP - i), gy + i, i, WB_DARK_GREY);
    }
}

/* Raise window to top of z-order */
static void raise_window(int wh)
{
    /* Find it in z-order */
    int pos = -1;
    for (int i = 0; i < g_nwins; i++) {
        if (g_zorder[i] == wh) { pos = i; break; }
    }
    if (pos < 0 || pos == g_nwins - 1) return;  /* already on top */

    /* Shift everything above it down one slot */
    for (int i = pos; i < g_nwins - 1; i++)
        g_zorder[i] = g_zorder[i + 1];
    g_zorder[g_nwins - 1] = wh;
}

/* Hit-test: returns window handle at (mx,my) in z-order (topmost first) */
static int hit_test(int mx, int my)
{
    for (int i = g_nwins - 1; i >= 0; i--) {
        int wh = g_zorder[i];
        WmWindow *w = &g_wins[wh];
        if (!w->active) continue;
        if (mx >= w->x && mx < w->x + w->w &&
            my >= w->y && my < w->y + w->h)
            return wh;
    }
    return -1;
}

/* Hit-test close gadget (14x14 box at top-left of title bar) */
static int hit_close_gadget(int wh, int mx, int my)
{
    WmWindow *w = &g_wins[wh];
    return (mx >= w->x + 3 && mx < w->x + 3 + 14 &&
            my >= w->y + 3 && my < w->y + 3 + 14);
}

/* Hit-test title bar only */
static int hit_titlebar(int wh, int mx, int my)
{
    WmWindow *w = &g_wins[wh];
    return (mx >= w->x && mx < w->x + w->w &&
            my >= w->y && my < w->y + WM_TITLEBAR_H);
}

/* Hit-test resize grip (bottom-right corner) */
static int hit_resize_grip(int wh, int mx, int my)
{
    WmWindow *w = &g_wins[wh];
    return (mx >= w->x + w->w - WM_RESIZE_GRIP &&
            my >= w->y + w->h - WM_RESIZE_GRIP &&
            mx <  w->x + w->w &&
            my <  w->y + w->h);
}

/* Repaint a single window in-place without full desktop repaint */
static void repaint_window(int wh)
{
    draw_chrome(wh);
    WmWindow *w = &g_wins[wh];
    if (w->draw)
        w->draw(w->x, w->y, w->w, w->h);
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int WM_AddWindow(int x, int y, int w, int h, const char *title,
                 WM_DrawFn draw, WM_KeyFn on_key)
{
    if (g_nwins >= WM_MAX_WINDOWS) return -1;

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!g_wins[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    WmWindow *win = &g_wins[slot];
    win->x       = x;
    win->y       = y;
    win->w       = w;
    win->h       = h;
    win->draw    = draw;
    win->on_key  = on_key;
    win->active  = 1;
    str_copy(win->title, title, 32);

    g_zorder[g_nwins++] = slot;

    /* New window gets focus */
    g_focus = slot;

    return slot;
}

void WM_MouseEvent(int mx, int my, int btn_left)
{
    int btn_pressed  = (btn_left && !g_btn_prev);
    int btn_released = (!btn_left && g_btn_prev);
    g_btn_prev = btn_left;

    extern unsigned int g_fb_width_irq;
    extern unsigned int g_fb_height_irq;

    if (btn_pressed) {
        int wh = hit_test(mx, my);
        if (wh < 0) {
            /* Missed all windows — pass to desktop (icon hit-test) */
            Desktop_MouseEvent(mx, my, 1);
        }
        if (wh >= 0) {
            /* Close gadget takes priority */
            if (hit_close_gadget(wh, mx, my)) {
                WM_CloseWindow(wh);
                return;
            }

            /* Focus and raise */
            int was_focused = (wh == g_focus);
            g_focus = wh;
            raise_window(wh);
            if (!was_focused)
                WM_Redraw();

            if (hit_resize_grip(wh, mx, my)) {
                /* Start resize */
                g_resize_handle  = wh;
                g_resize_base_w  = g_wins[wh].w;
                g_resize_base_h  = g_wins[wh].h;
                g_resize_orig_mx = mx;
                g_resize_orig_my = my;
            } else if (hit_titlebar(wh, mx, my)) {
                /* Start drag */
                g_drag_handle = wh;
                g_drag_off_x  = mx - g_wins[wh].x;
                g_drag_off_y  = my - g_wins[wh].y;
            }
        }
    }

    if (btn_left && g_drag_handle >= 0) {
        WmWindow *w = &g_wins[g_drag_handle];
        int new_x = mx - g_drag_off_x;
        int new_y = my - g_drag_off_y;

        /* Keep title bar reachable: at least 32px of it must stay on screen */
        int min_visible = 32;
        if (new_x > (int)g_fb_width_irq  - min_visible) new_x = (int)g_fb_width_irq  - min_visible;
        if (new_x < -(w->w - min_visible))               new_x = -(w->w - min_visible);
        if (new_y < 20) new_y = 20;  /* title bar must stay below menu bar */
        /* no bottom clamp — allow window to go off the bottom */

        if (new_x != w->x || new_y != w->y) {
            int old_x = w->x, old_y = w->y, old_w = w->w, old_h = w->h;
            w->x = new_x;
            w->y = new_y;
            Desktop_RedrawRect(old_x, old_y, old_w, old_h);
            for (int i = 0; i < g_nwins; i++)
                if (g_wins[g_zorder[i]].active) repaint_window(g_zorder[i]);
            Cursor_Redraw();
        }
    }

    if (btn_left && g_resize_handle >= 0) {
        WmWindow *w = &g_wins[g_resize_handle];
        int new_w = g_resize_base_w + (mx - g_resize_orig_mx);
        int new_h = g_resize_base_h + (my - g_resize_orig_my);

        if (new_w < 120) new_w = 120;
        if (new_h < 80)  new_h = 80;
        int max_w = (int)g_fb_width_irq  - w->x;
        int max_h = (int)g_fb_height_irq - w->y;
        if (new_w > max_w) new_w = max_w;
        if (new_h > max_h) new_h = max_h;

        if (new_w != w->w || new_h != w->h) {
            int old_w = w->w, old_h = w->h;
            w->w = new_w;
            w->h = new_h;
            Desktop_RedrawRect(w->x, w->y, old_w, old_h);
            for (int i = 0; i < g_nwins; i++)
                if (g_wins[g_zorder[i]].active) repaint_window(g_zorder[i]);
            Cursor_Redraw();
        }
    }

    if (btn_released) {
        g_drag_handle   = -1;
        g_resize_handle = -1;
    }
}

void WM_KeyEvent(char c)
{
    if (g_focus < 0) return;
    WmWindow *w = &g_wins[g_focus];
    if (w->active && w->on_key)
        w->on_key(c);
}

void WM_Redraw(void)
{
    /* Repaint full desktop backdrop */
    Desktop_Draw();

    /* Paint windows back-to-front */
    for (int i = 0; i < g_nwins; i++) {
        int wh = g_zorder[i];
        WmWindow *w = &g_wins[wh];
        if (!w->active) continue;
        draw_chrome(wh);
        if (w->draw)
            w->draw(w->x, w->y, w->w, w->h);
    }

    /* Restore cursor on top */
    Cursor_Redraw();
}

int WM_GetFocus(void)
{
    return g_focus;
}

void WM_CloseWindow(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    WmWindow *w = &g_wins[handle];
    if (!w->active) return;

    /* Save footprint before deactivating */
    int ox = w->x, oy = w->y, ow = w->w, oh = w->h;

    /* Remove from z-order array */
    int pos = -1;
    for (int i = 0; i < g_nwins; i++) {
        if (g_zorder[i] == handle) { pos = i; break; }
    }
    if (pos >= 0) {
        for (int i = pos; i < g_nwins - 1; i++)
            g_zorder[i] = g_zorder[i + 1];
        g_nwins--;
    }

    /* Free the slot */
    w->active = 0;

    /* Update focus to the new top window */
    if (g_focus == handle)
        g_focus = (g_nwins > 0) ? g_zorder[g_nwins - 1] : -1;

    /* Erase window footprint and repaint everything below it */
    Desktop_RedrawRect(ox, oy, ow + 2, oh + 2);
    for (int i = 0; i < g_nwins; i++) {
        int wh = g_zorder[i];
        if (g_wins[wh].active) repaint_window(wh);
    }
    Cursor_Redraw();
}

int WM_IsWindowActive(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return 0;
    return g_wins[handle].active;
}

void WM_MoveWindow(int handle, int new_x, int new_y)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    if (!g_wins[handle].active) return;
    g_wins[handle].x = new_x;
    g_wins[handle].y = new_y;
    WM_Redraw();
}
