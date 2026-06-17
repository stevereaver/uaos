/* wm.c — UAOS Window Manager */

#include "wm.h"
#include "framebuffer.h"
#include "cursor.h"
#include "desktop.h"
#include "filebrowser.h"
#include <stdint.h>
#include <stddef.h>

/* On-screen debug from filebrowser */
extern void dbg_add_line(const char *msg);

/* Debug output */
#define WM_DEBUG 1
#if WM_DEBUG
    #define WM_LOG(msg) do { extern void kprint(const char *); kprint(msg); } while(0)
    #define WM_LOG_DEC(v) do { extern void kprintdec(uint32_t); kprintdec((uint32_t)(v)); } while(0)
#else
    #define WM_LOG(msg) do {} while(0)
    #define WM_LOG_DEC(v) do {} while(0)
#endif

/* =========================================================================
 * Window registry and z-order
 * ========================================================================= */

static WmWindow g_wins[WM_MAX_WINDOWS];
static int      g_zorder[WM_MAX_WINDOWS];  /* indices into g_wins, [0]=back */
static int      g_nwins = 0;

static int g_focus    = -1;   /* index into g_wins of focused window   */

/* Set on btn_pressed when hit_test returns -1 (desktop press).
 * Desktop_MouseRelease is only forwarded when this flag is set, so that a
 * release after a window click never accidentally triggers desktop actions. */
static int g_press_was_desktop = 0;

/* Drag/resize state */
static int g_drag_handle  = -1;
static int g_drag_off_x   = 0;
static int g_drag_off_y   = 0;
static int g_resize_handle = -1;
static int g_resize_base_w = 0;
static int g_resize_base_h = 0;
static int g_resize_orig_mx = 0;
static int g_resize_orig_my = 0;
static int g_btn_prev     = 0;

/* Scrollbar drag state */
static int g_scroll_drag_win  = -1;  /* window whose thumb is being dragged */
static int g_scroll_drag_axis = 0;   /* 0=vert, 1=horiz */
static int g_scroll_drag_base = 0;   /* scroll value at drag start */
static int g_scroll_drag_mbase = 0;  /* mouse coord at drag start  */

/* =========================================================================
 * Helpers
 * ========================================================================= */

static void str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Forward declaration — needed by scroll_by which is defined before repaint_window */
static void repaint_window(int wh);

/* =========================================================================
 * Chrome geometry helpers
 * ========================================================================= */

#define SB  WM_SCROLLBAR_W   /* scrollbar / border thickness shorthand */

/* Right scrollbar rect */
static void sb_right_rect(WmWindow *w, int *x, int *y, int *wd, int *ht)
{
    *x  = w->x + w->w - SB;
    *y  = w->y + WM_TITLEBAR_H;
    *wd = SB;
    *ht = w->h - WM_TITLEBAR_H - SB;  /* full height minus title and resize grip */
    if (*ht < 0) *ht = 0;
}

/* Bottom scrollbar rect */
static void sb_bottom_rect(WmWindow *w, int *x, int *y, int *wd, int *ht)
{
    *x  = w->x + 1;
    *y  = w->y + w->h - SB;
    *wd = w->w - 1 - SB;   /* stops left of resize grip */
    *ht = SB;
}

/* Client area: 1px left inset (outline only), right inset = SB (scrollbar) */
static void client_rect(WmWindow *w, int *cx, int *cy, int *cw, int *ch)
{
    *cx = w->x + 1;
    *cy = w->y + WM_TITLEBAR_H;
    *cw = w->w - 1 - SB;   /* right edge is scrollbar */
    *ch = w->h - WM_TITLEBAR_H - SB; /* bottom edge is scrollbar */
}

/* =========================================================================
 * Scrollbar drawing helpers
 * ========================================================================= */

/* Draw a small directional arrow triangle in a box */
static void draw_arrow(int bx, int by, int bw, int bh,
                        int dir, uint32_t bg)
{
    /* dir: 0=up 1=down 2=left 3=right */
    FB_FillRect(bx, by, bw, bh, bg);
    FB_DrawRect(bx, by, bw, bh, WB_DARK_GREY);
    int mx = bx + bw / 2;
    int my = by + bh / 2;
    int sz = (bw < bh ? bw : bh) / 2 - 2;
    if (sz < 1) sz = 1;
    for (int i = 0; i <= sz; i++) {
        if (dir == 0) /* up */
            FB_DrawHLine(mx - i, my - sz + i, i * 2 + 1, WB_DARK_GREY);
        else if (dir == 1) /* down */
            FB_DrawHLine(mx - i, my + sz - i, i * 2 + 1, WB_DARK_GREY);
        else if (dir == 2) /* left */
            FB_DrawVLine(mx - sz + i, my - i, i * 2 + 1, WB_DARK_GREY);
        else /* right */
            FB_DrawVLine(mx + sz - i, my - i, i * 2 + 1, WB_DARK_GREY);
    }
}

/* Draw a scrollbar (vertical or horizontal).
 * track_x/y/w/h: the full scrollbar rectangle
 * arrow_sz: size of each arrow button (square)
 * scroll: current offset, content_sz: total content size, view_sz: view size
 * axis: 0=vertical, 1=horizontal */
static void draw_scrollbar(int tx, int ty, int tw, int th,
                            int arrow_sz,
                            int scroll, int content_sz, int view_sz,
                            int axis)
{
    uint32_t bg = WB_GREY;

    /* Clear entire scrollbar area including full interior to prevent ANY artifacts */
    FB_FillRect(tx, ty, tw, th, WB_GREY);

    if (axis == 0) { /* vertical */
        /* Track area between arrows - clear first */
        int track_y = ty + arrow_sz;
        int track_h = th - arrow_sz * 2;
        if (track_h > 0) {
            FB_FillRect(tx, track_y, tw, track_h, WB_GREY);
        }

        /* Up arrow */
        draw_arrow(tx, ty, tw, arrow_sz, 0, bg);
        /* Down arrow — only if track is tall enough */
        if (th >= arrow_sz * 2)
            draw_arrow(tx, ty + th - arrow_sz, tw, arrow_sz, 1, bg);

        /* Draw thumb if scrollable and track is tall enough */
        if (track_h > 4 && content_sz > view_sz && scroll >= 0) {
            int thumb_h = track_h * view_sz / content_sz;
            if (thumb_h < 8) thumb_h = 8;
            if (thumb_h > track_h) thumb_h = track_h;
            int range = content_sz - view_sz;
            int thumb_y = track_y + (range > 0 ? (track_h - thumb_h) * scroll / range : 0);
            /* Clamp thumb within track */
            if (thumb_y < track_y) thumb_y = track_y;
            if (thumb_y + thumb_h > track_y + track_h) thumb_y = track_y + track_h - thumb_h;
            FB_FillRect(tx + 1, thumb_y, tw - 2, thumb_h, WB_LIGHT_GREY);
            FB_DrawRect(tx + 1, thumb_y, tw - 2, thumb_h, WB_DARK_GREY);
        }

        /* Draw outer border last to ensure it's clean */
        FB_DrawRect(tx, ty, tw, th, WB_DARK_GREY);
    } else { /* horizontal */
        /* Track area between arrows - clear first */
        int track_x = tx + arrow_sz;
        int track_w = tw - arrow_sz * 2;
        if (track_w > 0) {
            FB_FillRect(track_x, ty, track_w, th, WB_GREY);
        }

        /* Left arrow */
        draw_arrow(tx, ty, arrow_sz, th, 2, bg);
        /* Right arrow */
        draw_arrow(tx + tw - arrow_sz, ty, arrow_sz, th, 3, bg);

        /* Draw thumb if scrollable and track is wide enough */
        if (track_w > 4 && content_sz > view_sz) {
            int thumb_w = track_w * view_sz / content_sz;
            if (thumb_w < 8) thumb_w = 8;
            if (thumb_w > track_w) thumb_w = track_w;
            int thumb_x = track_x + (track_w - thumb_w) * scroll
                          / (content_sz - view_sz);
            FB_FillRect(thumb_x, ty + 1, thumb_w, th - 2, WB_LIGHT_GREY);
            FB_DrawRect(thumb_x, ty + 1, thumb_w, th - 2, WB_DARK_GREY);
        }

        /* Draw outer border last to ensure it's clean */
        FB_DrawRect(tx, ty, tw, th, WB_DARK_GREY);
    }
}

/* Draw a single window chrome (title bar + borders + scrollbars) */
static void draw_chrome(int wh)
{
    WmWindow *w = &g_wins[wh];
    int focused = (wh == g_focus);
    uint32_t tbar_col = focused ? WB_LIGHT_BLUE : WB_BLUE;

    /* Outer outline */
    FB_DrawRect(w->x, w->y, w->w, w->h, WB_DARK_GREY);

    /* Title bar fill — full width inside the outer outline, down to client edge */
    FB_FillRect(w->x + 1, w->y + 1,
                w->w - 2, WM_TITLEBAR_H - 1, tbar_col);

    /* Title text centred in bar */
    FB_PutStrCentred(w->x + 1, w->y + 1,
                     w->w - 2, WM_TITLEBAR_H - 1,
                     w->title, WB_WHITE, tbar_col);

    /* Close gadget — flush to top-left corner of window with X symbol */
    int cg_x = w->x + 1;
    int cg_y = w->y + 1;
    FB_DrawRect(cg_x, cg_y, 14, 14, WB_WHITE);
    FB_FillRect(cg_x + 1, cg_y + 1, 12, 12, tbar_col);
    
    /* Draw X symbol in close gadget */
    uint32_t x_col = focused ? WB_WHITE : WB_LIGHT_GREY;
    /* Diagonal from top-left to bottom-right */
    for (int i = 0; i < 10; i++) {
        FB_PutPixel(cg_x + 3 + i, cg_y + 3 + i, x_col);
    }
    /* Diagonal from top-right to bottom-left */
    for (int i = 0; i < 10; i++) {
        FB_PutPixel(cg_x + 12 - i, cg_y + 3 + i, x_col);
    }

    /* Zoom gadget — second from right: upward-pointing arrow box (Amiga style) */
    int zg_x = w->x + w->w - 30;   /* 15px wide, 15px left of depth gadget */
    int zg_y = w->y + 1;
    FB_DrawRect(zg_x, zg_y, 14, 14, WB_WHITE);
    FB_FillRect(zg_x + 1, zg_y + 1, 12, 12, tbar_col);
    if (w->zoomed) {
        /* Restore icon: downward-pointing arrow (shrink back) */
        int mx2 = zg_x + 7;
        int ty  = zg_y + 9;
        for (int r = 0; r < 4; r++)
            FB_DrawHLine(mx2 - r, ty - r, r * 2 + 1, WB_WHITE);
    } else {
        /* Zoom icon: upward-pointing arrow (maximise) */
        int mx2 = zg_x + 7;
        int ty  = zg_y + 3;
        for (int r = 0; r < 4; r++)
            FB_DrawHLine(mx2 - r, ty + r, r * 2 + 1, WB_WHITE);
    }

    /* Depth gadget — top-right corner: two overlapping rectangles (Amiga style) */
    int dg_x = w->x + w->w - 15;
    int dg_y = w->y + 1;
    /* Back layer (larger rect, offset right+down) */
    FB_DrawRect(dg_x + 3, dg_y,     10, 10, WB_WHITE);
    FB_FillRect(dg_x + 4, dg_y + 1,  8,  8, tbar_col);
    /* Front layer (smaller rect, offset left+up) */
    FB_DrawRect(dg_x,     dg_y + 3, 10, 10, WB_WHITE);
    FB_FillRect(dg_x + 1, dg_y + 4,  8,  8, tbar_col);

    /* Window body background */
    int cx, cy, cw, ch;
    client_rect(w, &cx, &cy, &cw, &ch);
    FB_FillRect(cx, cy, cw, ch, WB_GREY);

    /* Right scrollbar */
    int rx, ry, rw, rh;
    sb_right_rect(w, &rx, &ry, &rw, &rh);
    int sv = w->content_h > ch ? w->content_h : ch + 1;
    if (w->scroll_y < 0) w->scroll_y = 0;
    draw_scrollbar(rx, ry, rw, rh, SB,
                   w->scroll_y, sv, ch, 0);

    /* Bottom scrollbar */
    int bx, by, bw, bh;
    sb_bottom_rect(w, &bx, &by, &bw, &bh);
    int sh = (w->content_w > 0) ? w->content_w : (cw + 1);
    draw_scrollbar(bx, by, bw, bh, SB,
                   w->scroll_x, sh, cw, 1);

    /* Resize grip — SB×SB square in bottom-right corner
     * Clear area first, then draw border and diagonal stripes */
    {
        int gx = w->x + w->w - SB;
        int gy = w->y + w->h - SB;
        /* Clear entire grip area including inner pixels to prevent artifacts */
        FB_FillRect(gx, gy, SB, SB, WB_GREY);
        FB_DrawRect(gx, gy, SB, SB, WB_DARK_GREY);
        /* Two diagonal stripes (drawn within the border, so offset by 1) */
        for (int row = 2; row < SB - 1; row++) {
            int c1 = SB - 2 - row;
            int c2 = SB - 4 - row;
            if (c1 >= 1 && c1 < SB - 1)
                FB_PutPixel(gx + c1, gy + row, WB_DARK_GREY);
            if (c2 >= 1 && c2 < SB - 1)
                FB_PutPixel(gx + c2, gy + row, WB_WHITE);
        }
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

/* Hit-test close gadget */
static int hit_close_gadget(int wh, int mx, int my)
{
    WmWindow *w = &g_wins[wh];
    int cg_x = w->x + 1;
    int cg_y = w->y + 1;
    return (mx >= cg_x && mx < cg_x + 14 &&
            my >= cg_y && my < cg_y + 14);
}

/* Hit-test title bar (whole row, used for drag) */
static int hit_titlebar(int wh, int mx, int my)
{
    WmWindow *w = &g_wins[wh];
    return (mx >= w->x && mx < w->x + w->w &&
            my >= w->y && my < w->y + WM_TITLEBAR_H);
}

/* Hit-test zoom gadget (second from right in title bar) */
static int hit_zoom_gadget(int wh, int mx, int my)
{
    WmWindow *w = &g_wins[wh];
    int zg_x = w->x + w->w - 30;
    int zg_y = w->y + 1;
    return (mx >= zg_x && mx < zg_x + 14 &&
            my >= zg_y && my < zg_y + 14);
}

/* Toggle zoom: maximise to full usable screen or restore saved geometry */
static void zoom_window(int wh)
{
    WmWindow *w = &g_wins[wh];
    if (w->zoomed) {
        /* Restore */
        w->x = w->restore_x;
        w->y = w->restore_y;
        w->w = w->restore_w;
        w->h = w->restore_h;
        w->zoomed = 0;
    } else {
        /* Save current geometry and maximise */
        w->restore_x = w->x;
        w->restore_y = w->y;
        w->restore_w = w->w;
        w->restore_h = w->h;
        w->x = 0;
        w->y = 20;   /* below menu bar */
        w->w = (int)g_fb.width;
        w->h = (int)g_fb.height - 20;
        w->zoomed = 1;
    }
}

/* Hit-test depth gadget (top-right of title bar) */
static int hit_depth_gadget(int wh, int mx, int my)
{
    WmWindow *w = &g_wins[wh];
    int dg_x = w->x + w->w - 15;
    int dg_y = w->y + 1;
    return (mx >= dg_x && mx < dg_x + 13 &&
            my >= dg_y && my < dg_y + 13);
}

/* Cycle window to back of z-order (send behind all others) */
static void depth_window(int wh)
{
    /* Find position of wh in g_zorder */
    int pos = -1;
    for (int i = 0; i < g_nwins; i++)
        if (g_zorder[i] == wh) { pos = i; break; }
    if (pos < 0) return;

    if (pos == 0) {
        /* Already at back — bring to front */
        for (int i = 0; i < g_nwins - 1; i++)
            g_zorder[i] = g_zorder[i + 1];
        g_zorder[g_nwins - 1] = wh;
    } else {
        /* Send to back */
        for (int i = pos; i > 0; i--)
            g_zorder[i] = g_zorder[i - 1];
        g_zorder[0] = wh;
    }

    /* Focus shifts to the new topmost window */
    g_focus = g_zorder[g_nwins - 1];
}

/* Hit-test resize grip (bottom-right SB×SB square) */
static int hit_resize_grip(int wh, int mx, int my)
{
    WmWindow *w = &g_wins[wh];
    return (mx >= w->x + w->w - SB && mx < w->x + w->w &&
            my >= w->y + w->h - SB && my < w->y + w->h);
}

/* Scroll by one unit and redraw, clamped to [0, content-view] */
static void scroll_by(int wh, int axis, int delta)
{
    WmWindow *w = &g_wins[wh];
    int cx, cy, cw, ch;
    client_rect(w, &cx, &cy, &cw, &ch);
    if (axis == 0) { /* vertical */
        int vh = (w->view_h > 0) ? w->view_h : ch;
        int max_s = (w->content_h > vh) ? w->content_h - vh : 0;
        w->scroll_y += delta;
        if (w->scroll_y < 0) w->scroll_y = 0;
        if (w->scroll_y > max_s) w->scroll_y = max_s;
    } else { /* horizontal */
        int max_s = (w->content_w > cw) ? w->content_w - cw : 0;
        w->scroll_x += delta;
        if (w->scroll_x < 0) w->scroll_x = 0;
        if (w->scroll_x > max_s) w->scroll_x = max_s;
    }
    WM_Redraw();
}

/* Hit-test scrollbar arrows/thumb, returning scroll delta or 0 */
/* Returns 1 if the click was on a scrollbar element (consumed) */
static int hit_scrollbars(int wh, int mx, int my)
{
    WmWindow *w = &g_wins[wh];
    int cx, cy, cw, ch;
    client_rect(w, &cx, &cy, &cw, &ch);

    /* Right (vertical) scrollbar */
    int rx, ry, rw, rh;
    sb_right_rect(w, &rx, &ry, &rw, &rh);
    if (mx >= rx && mx < rx + rw && my >= ry && my < ry + rh) {
        if (my < ry + SB) { scroll_by(wh, 0, -16); return 1; } /* up arrow: scroll 1 line up */
        if (my >= ry + rh - SB) { scroll_by(wh, 0, 16); return 1; } /* down arrow: scroll 1 line down */
        /* Thumb track drag start */
        g_scroll_drag_win  = wh;
        g_scroll_drag_axis = 0;
        g_scroll_drag_base = w->scroll_y;
        g_scroll_drag_mbase = my;
        return 1;
    }

    /* Bottom (horizontal) scrollbar */
    int bx, by, bw, bh;
    sb_bottom_rect(w, &bx, &by, &bw, &bh);
    if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
        if (mx < bx + SB) { scroll_by(wh, 1, -16); return 1; } /* left arrow: small scroll */
        if (mx >= bx + bw - SB) { scroll_by(wh, 1, 16); return 1; } /* right arrow: small scroll */
        g_scroll_drag_win  = wh;
        g_scroll_drag_axis = 1;
        g_scroll_drag_base = w->scroll_x;
        g_scroll_drag_mbase = mx;
        return 1;
    }
    return 0;
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

    /* WM_LOG disabled — causes hang when called at runtime from non-boot context */

    WmWindow *win = &g_wins[slot];
    win->x       = x;
    win->y       = y;
    win->w       = w;
    win->h       = h;
    win->draw     = draw;
    win->on_key   = on_key;
    win->on_click   = (WM_ClickFn)0;
    win->on_move    = (WM_MouseMoveFn)0;
    win->on_release = (WM_MouseReleaseFn)0;
    win->scroll_x   = 0;
    win->scroll_y   = 0;
    win->content_w  = 0;
    win->content_h  = 0;
    win->active     = 1;
    str_copy(win->title, title, 32);

    g_zorder[g_nwins++] = slot;

    /* New window gets focus */
    g_focus = slot;

    return slot;
}

void WM_SetMouseMoveHandler(int handle, WM_MouseMoveFn on_move)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    g_wins[handle].on_move = on_move;
}

void WM_SetMouseReleaseHandler(int handle, WM_MouseReleaseFn on_release)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    g_wins[handle].on_release = on_release;
}

void WM_MouseEvent(int mx, int my, int btn_left)
{
    int btn_pressed  = (btn_left && !g_btn_prev);
    int btn_released = (!btn_left && g_btn_prev);
    g_btn_prev = btn_left;

    if (btn_pressed) {
        WM_LOG("[WM] Mouse press at "); WM_LOG_DEC(mx); WM_LOG(","); WM_LOG_DEC(my); WM_LOG("\n");
        int wh = hit_test(mx, my);
        g_press_was_desktop = (wh < 0);
        if (wh < 0) {
            /* Missed all windows — pass to desktop (icon hit-test) */
            WM_LOG("[WM] Missed windows, sending to desktop\n");
            Desktop_MouseEvent(mx, my, 1);
            return;
        }
        WM_LOG("[WM] Hit window "); WM_LOG_DEC(wh); WM_LOG("\n");
        if (wh >= 0) {
            /* Cancel pending double-click state in all browsers except the
             * one being clicked.  A stale first-click in a background browser
             * could otherwise complete as a spurious double-click the next
             * time that browser's on_click fires (e.g. clicking Clock in
             * Tools opens DEVS in the Workbench: browser underneath). */
            FileBrowser_CancelClicks(wh);

            /* Close gadget takes priority */
            if (hit_close_gadget(wh, mx, my)) {
                WM_CloseWindow(wh);
                return;
            }

            /* Zoom gadget */
            if (hit_zoom_gadget(wh, mx, my)) {
                zoom_window(wh);
                WM_Redraw();
                return;
            }

            /* Depth gadget — check before focus/raise */
            if (hit_depth_gadget(wh, mx, my)) {
                depth_window(wh);
                WM_Redraw();
                return;
            }

            /* Focus and raise */
            int was_focused = (wh == g_focus);
            g_focus = wh;
            raise_window(wh);
            if (!was_focused)
                WM_Redraw();

            if (hit_resize_grip(wh, mx, my)) {
                g_resize_handle  = wh;
                g_resize_base_w  = g_wins[wh].w;
                g_resize_base_h  = g_wins[wh].h;
                g_resize_orig_mx = mx;
                g_resize_orig_my = my;
            } else if (hit_titlebar(wh, mx, my)) {
                g_drag_handle = wh;
                g_drag_off_x  = mx - g_wins[wh].x;
                g_drag_off_y  = my - g_wins[wh].y;
            } else if (hit_scrollbars(wh, mx, my)) {
                /* consumed by scrollbar */
            } else if (g_wins[wh].on_click) {
                /* Save focus before the click handler; the handler may open
                 * a new window which changes g_focus.  We must return after
                 * on_click so the new window layout isn't immediately hit-tested
                 * against the same button-press event (which could spuriously
                 * trigger the new window's close gadget or other gadgets). */
                g_wins[wh].on_click(wh, mx, my);
                return;
            }
        }
    }

    if (btn_left && g_drag_handle >= 0) {
        WmWindow *w = &g_wins[g_drag_handle];
        int new_x = mx - g_drag_off_x;
        int new_y = my - g_drag_off_y;

        /* Keep title bar reachable: at least 32px of it must stay on screen */
        int min_visible = 32;
        if (new_x > (int)g_fb.width  - min_visible) new_x = (int)g_fb.width  - min_visible;
        if (new_x < -(w->w - min_visible))               new_x = -(w->w - min_visible);
        if (new_y < 20) new_y = 20;  /* title bar must stay below menu bar */
        /* no bottom clamp — allow window to go off the bottom */

        if (new_x != w->x || new_y != w->y) {
            w->x = new_x;
            w->y = new_y;
            WM_Redraw();
        }
    }

    if (btn_left && g_resize_handle >= 0) {
        WmWindow *w = &g_wins[g_resize_handle];
        int new_w = g_resize_base_w + (mx - g_resize_orig_mx);
        int new_h = g_resize_base_h + (my - g_resize_orig_my);

        /* Enforce minimum size: shell needs at least titlebar + inputbar + some history */
        if (new_w < 200) new_w = 200;
        if (new_h < 120) new_h = 120;
        int max_w = (int)g_fb.width  - w->x;
        int max_h = (int)g_fb.height - w->y;
        if (new_w > max_w) new_w = max_w;
        if (new_h > max_h) new_h = max_h;

        if (new_w != w->w || new_h != w->h) {
            w->w = new_w;
            w->h = new_h;
            WM_Redraw();
        }
    }

    /* Scrollbar thumb drag */
    if (btn_left && g_scroll_drag_win >= 0) {
        int wh = g_scroll_drag_win;
        WmWindow *w = &g_wins[wh];
        int cx, cy, cw, ch;
        client_rect(w, &cx, &cy, &cw, &ch);

        if (g_scroll_drag_axis == 0) { /* vertical */
            int rx, ry, rw, rh;
            sb_right_rect(w, &rx, &ry, &rw, &rh);
            int track_h = rh - SB * 2;
            int vh = (w->view_h > 0) ? w->view_h : ch;
            int sv = (w->content_h > 0) ? w->content_h : vh;
            if (track_h > 0 && sv > vh) {
                int dm = my - g_scroll_drag_mbase;
                int max_s = sv - vh;
                int new_s = g_scroll_drag_base + dm * max_s / track_h;
                if (new_s < 0) new_s = 0;
                if (new_s > max_s) new_s = max_s;
                if (new_s != w->scroll_y) {
                    w->scroll_y = new_s;
                    WM_Redraw();
                }
            }
        } else { /* horizontal */
            int bx, by, bw, bh;
            sb_bottom_rect(w, &bx, &by, &bw, &bh);
            int track_w = bw - SB * 2;
            int sh = (w->content_w > 0) ? w->content_w : cw;
            if (track_w > 0 && sh > cw) {
                int dm = mx - g_scroll_drag_mbase;
                int max_s = sh - cw;
                int new_s = g_scroll_drag_base + dm * max_s / track_w;
                if (new_s < 0) new_s = 0;
                if (new_s > max_s) new_s = max_s;
                if (new_s != w->scroll_x) {
                    w->scroll_x = new_s;
                    WM_Redraw();
                }
            }
        }
    }

    /* Window client-area mouse move */
    if (btn_left && g_drag_handle < 0 && g_resize_handle < 0 && g_scroll_drag_win < 0) {
        if (g_focus >= 0) {
            WmWindow *w = &g_wins[g_focus];
            if (w->active && w->on_move)
                w->on_move(g_focus, mx, my);
        }
    }

    /* Desktop icon drag — only when this gesture started on the desktop */
    if (g_press_was_desktop && btn_left &&
        g_drag_handle < 0 && g_resize_handle < 0 && g_scroll_drag_win < 0) {
        Desktop_MouseMove(mx, my, 1);
    }

    if (btn_released) {
        if (g_focus >= 0) {
            WmWindow *w = &g_wins[g_focus];
            if (w->active && w->on_release)
                w->on_release(g_focus, mx, my);
        }
        g_drag_handle      = -1;
        g_resize_handle    = -1;
        g_scroll_drag_win  = -1;
        /* Only forward the release to the desktop if the press also landed on
         * the desktop.  If the press hit a window, Desktop_MouseRelease must
         * not fire — it could misfire a stale g_icon_drag_idx as a double-click
         * on a desktop icon (e.g. opening Workbench:DEVS after clicking Clock). */
        if (g_press_was_desktop)
            Desktop_MouseRelease(mx, my);
        g_press_was_desktop = 0;
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
    FB_BeginDraw();

    /* Repaint full desktop backdrop only if Workbench has been loaded */
    if (Desktop_IsWorkbenchLoaded())
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

    /* Cursor on top, then flip entire frame to screen in one blit */
    Cursor_Redraw();
    FB_Flip();
}

int WM_GetFocus(void)
{
    return g_focus;
}

void WM_RequestFocus(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    if (!g_wins[handle].active) return;
    
    int was_focused = (handle == g_focus);
    g_focus = handle;
    raise_window(handle);
    if (!was_focused)
        WM_Redraw();
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
    Cursor_Hide();
    Desktop_RedrawRect(ox, oy, ow + 2, oh + 2);
    for (int i = 0; i < g_nwins; i++) {
        int wh = g_zorder[i];
        if (g_wins[wh].active) repaint_window(wh);
    }
    Cursor_Redraw();
}

void WM_SetClickHandler(int handle, WM_ClickFn on_click)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    g_wins[handle].on_click = on_click;
}

void WM_SetScrollInfo(int handle, int content_w, int content_h)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    g_wins[handle].content_w = content_w;
    g_wins[handle].content_h = content_h;
}

void WM_SetScrollInfoEx(int handle, int content_w, int content_h, int view_h)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    g_wins[handle].content_w = content_w;
    g_wins[handle].content_h = content_h;
    g_wins[handle].view_h    = view_h;
}

int WM_GetScrollX(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return 0;
    return g_wins[handle].scroll_x;
}

int WM_GetScrollY(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return 0;
    return g_wins[handle].scroll_y;
}

void WM_SetScrollY(int handle, int y)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    WmWindow *w = &g_wins[handle];
    int cx, cy, cw, ch;
    client_rect(w, &cx, &cy, &cw, &ch);
    /* Use content-supplied view_h if set, otherwise fall back to client ch */
    int vh = (w->view_h > 0) ? w->view_h : ch;
    int sv = (w->content_h > vh) ? w->content_h : vh;
    int max_s = sv - vh;  /* always >= 0 */
    if (y < 0) y = 0;
    if (y > max_s) y = max_s;
    w->scroll_y = y;
}

int WM_IsWindowActive(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return 0;
    return g_wins[handle].active;
}

WM_DrawFn WM_GetDrawFn(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return 0;
    if (!g_wins[handle].active) return 0;
    return g_wins[handle].draw;
}

void WM_RaiseWindow(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    if (!g_wins[handle].active) return;
    raise_window(handle);
    g_focus = handle;
}

void WM_MoveWindow(int handle, int new_x, int new_y)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    if (!g_wins[handle].active) return;
    g_wins[handle].x = new_x;
    g_wins[handle].y = new_y;
    WM_Redraw();
}

int WM_GetWindowTitle(int handle, char *out, int max)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS || !g_wins[handle].active)
        return 0;
    int i = 0;
    while (i < max - 1 && g_wins[handle].title[i]) {
        out[i] = g_wins[handle].title[i];
        i++;
    }
    out[i] = '\0';
    return 1;
}
