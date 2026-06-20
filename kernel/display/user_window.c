/* user_window.c — UAOS userspace GUI window implementation
 *
 * Backs each userspace window with a kernel-side pixel buffer.  The user task
 * draws text/rects into the buffer via syscalls; the window manager blits the
 * buffer to the screen when the window is repainted.
 */

#include "user_window.h"
#include "framebuffer.h"
#include "wm.h"
#include "cursor.h"
#include "../exec/elf64_loader.h"
#include "../exec/task.h"
#include "../boot/kprint.h"
#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Per-window state
 * ------------------------------------------------------------------------- */
typedef struct {
    int                 active;
    int                 wm_handle;
    uint32_t           *buf;
    int                 buf_w;
    int                 buf_h;
    int                 dirty;
    int                 last_scroll_x;
    int                 last_scroll_y;

    /* Event ring buffer */
    struct uaos_gui_event events[UWIN_MAX_EVENT];
    int                 ev_head;
    int                 ev_tail;
    int                 ev_count;
} UserWindow;

static UserWindow g_uwins[UWIN_MAX_WINDOWS];

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */
static void str16_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int find_free_handle(void)
{
    for (int i = 0; i < UWIN_MAX_WINDOWS; i++) {
        if (!g_uwins[i].active)
            return i;
    }
    return -1;
}

static int wm_to_uwin(int wm_handle)
{
    for (int i = 0; i < UWIN_MAX_WINDOWS; i++) {
        if (g_uwins[i].active && g_uwins[i].wm_handle == wm_handle)
            return i;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Buffer drawing primitives
 * ------------------------------------------------------------------------- */
static inline void buf_pixel(UserWindow *u, int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= u->buf_w || y >= u->buf_h)
        return;
    u->buf[y * u->buf_w + x] = c;
}

static void buf_fill_rect(UserWindow *u, int x, int y, int w, int h, uint32_t c)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > u->buf_w) x1 = u->buf_w;
    int y1 = y + h; if (y1 > u->buf_h) y1 = u->buf_h;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (int row = y0; row < y1; row++) {
        uint32_t *p = u->buf + row * u->buf_w + x0;
        for (int col = x0; col < x1; col++)
            *p++ = c;
    }
}

static void buf_put_char(UserWindow *u, int x, int y, char ch, uint32_t fg, uint32_t bg)
{
    unsigned char c = (unsigned char)ch;
    if (c < 0x20 || c > 0x7E)
        c = 0x20;
    const uint8_t *glyph = g_font8x16[c - 0x20];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            int py = y + row;
            if (px < 0 || py < 0 || px >= u->buf_w || py >= u->buf_h)
                continue;
            u->buf[py * u->buf_w + px] = (bits & (0x80 >> col)) ? fg : bg;
        }
    }
}

static void buf_put_str(UserWindow *u, int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
    int cx = x;
    while (*s) {
        buf_put_char(u, cx, y, *s, fg, bg);
        cx += 8;
        s++;
    }
}

/* -------------------------------------------------------------------------
 * Window manager draw callback
 * ------------------------------------------------------------------------- */
static void uwin_draw(int win_x, int win_y, int win_w, int win_h)
{
    (void)win_w; (void)win_h;
    int handle = wm_to_uwin(WM_CurrentDrawHandle);
    if (handle < 0)
        return;
    UserWindow *u = &g_uwins[handle];

    /* Detect WM-driven scroll changes and notify the owner. */
    int sx = WM_GetScrollX(u->wm_handle);
    int sy = WM_GetScrollY(u->wm_handle);
    if (sx != u->last_scroll_x || sy != u->last_scroll_y) {
        u->last_scroll_x = sx;
        u->last_scroll_y = sy;
        if (u->ev_count < UWIN_MAX_EVENT) {
            struct uaos_gui_event *e = &u->events[u->ev_tail];
            e->type = UWIN_EVENT_SCROLL;
            e->button = 0;
            e->x = (int16_t)sx;
            e->y = (int16_t)sy;
            u->ev_tail = (u->ev_tail + 1) % UWIN_MAX_EVENT;
            u->ev_count++;
        }
    }

    if (!u->buf)
        return;

    /* Compute client area the same way WM does. */
    int cx = win_x + 1;
    int cy = win_y + WM_TITLEBAR_H;
    int cw = win_w - 1 - WM_SCROLLBAR_W;
    int ch = win_h - WM_TITLEBAR_H - WM_SCROLLBAR_W;
    if (cw < 0) cw = 0;
    if (ch < 0) ch = 0;

    int blit_w = cw < u->buf_w ? cw : u->buf_w;
    int blit_h = ch < u->buf_h ? ch : u->buf_h;
    if (blit_w <= 0 || blit_h <= 0)
        return;

    for (int row = 0; row < blit_h; row++) {
        uint32_t *src = u->buf + row * u->buf_w;
        for (int col = 0; col < blit_w; col++) {
            int px = cx + col;
            int py = cy + row;
            if (px >= 0 && py >= 0 && (unsigned)px < g_fb.width && (unsigned)py < g_fb.height)
                FB_PutPixel(px, py, src[col]);
        }
    }

    u->dirty = 0;
}

/* -------------------------------------------------------------------------
 * Event handlers (enqueued for the user task)
 * ------------------------------------------------------------------------- */
static void screen_to_client(int wm_handle, int mx, int my, int *rx, int *ry)
{
    int wx, wy;
    if (WM_GetWindowRect(wm_handle, &wx, &wy, NULL, NULL)) {
        *rx = mx - (wx + 1);
        *ry = my - (wy + WM_TITLEBAR_H);
    } else {
        *rx = mx;
        *ry = my;
    }
}

static void uwin_on_click(int wm_handle, int mx, int my)
{
    int handle = wm_to_uwin(wm_handle);
    if (handle < 0)
        return;
    UserWindow *u = &g_uwins[handle];

    int rx, ry;
    screen_to_client(wm_handle, mx, my, &rx, &ry);

    if (u->ev_count < UWIN_MAX_EVENT) {
        struct uaos_gui_event *e = &u->events[u->ev_tail];
        e->type = UWIN_EVENT_CLICK;
        e->button = 1;
        e->x = (int16_t)rx;
        e->y = (int16_t)ry;
        u->ev_tail = (u->ev_tail + 1) % UWIN_MAX_EVENT;
        u->ev_count++;
    }
}

static void uwin_on_move(int wm_handle, int mx, int my)
{
    int handle = wm_to_uwin(wm_handle);
    if (handle < 0)
        return;
    UserWindow *u = &g_uwins[handle];

    int rx, ry;
    screen_to_client(wm_handle, mx, my, &rx, &ry);

    if (u->ev_count < UWIN_MAX_EVENT) {
        struct uaos_gui_event *e = &u->events[u->ev_tail];
        e->type = UWIN_EVENT_MOVE;
        e->button = 1;
        e->x = (int16_t)rx;
        e->y = (int16_t)ry;
        u->ev_tail = (u->ev_tail + 1) % UWIN_MAX_EVENT;
        u->ev_count++;
    }
}

static void uwin_on_release(int wm_handle, int mx, int my)
{
    int handle = wm_to_uwin(wm_handle);
    if (handle < 0)
        return;
    UserWindow *u = &g_uwins[handle];

    int rx, ry;
    screen_to_client(wm_handle, mx, my, &rx, &ry);

    if (u->ev_count < UWIN_MAX_EVENT) {
        struct uaos_gui_event *e = &u->events[u->ev_tail];
        e->type = UWIN_EVENT_RELEASE;
        e->button = 0;
        e->x = (int16_t)rx;
        e->y = (int16_t)ry;
        u->ev_tail = (u->ev_tail + 1) % UWIN_MAX_EVENT;
        u->ev_count++;
    }
}

static void uwin_on_key(char c)
{
    int wh = WM_GetFocus();
    if (wh < 0)
        return;
    int handle = wm_to_uwin(wh);
    if (handle < 0)
        return;
    UserWindow *u = &g_uwins[handle];

    if (u->ev_count < UWIN_MAX_EVENT) {
        struct uaos_gui_event *e = &u->events[u->ev_tail];
        e->type = UWIN_EVENT_KEY;
        e->button = 0;
        e->x = (int16_t)(unsigned char)c;
        e->y = 0;
        u->ev_tail = (u->ev_tail + 1) % UWIN_MAX_EVENT;
        u->ev_count++;
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
void UserWindow_Init(void)
{
    for (int i = 0; i < UWIN_MAX_WINDOWS; i++) {
        g_uwins[i].active = 0;
        g_uwins[i].wm_handle = -1;
        g_uwins[i].buf = NULL;
        g_uwins[i].ev_head = 0;
        g_uwins[i].ev_tail = 0;
        g_uwins[i].ev_count = 0;
    }
}

int UserWindow_Create(const char *title, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0 || !title)
        return -1;

    int handle = find_free_handle();
    if (handle < 0)
        return -1;

    char short_title[UWIN_MAX_TITLE];
    str16_copy(short_title, title, UWIN_MAX_TITLE);

    int wm_handle = WM_AddWindow(x, y, w, h, short_title, uwin_draw, uwin_on_key);
    if (wm_handle < 0)
        return -1;

    WM_SetClickHandler(wm_handle, uwin_on_click);
    WM_SetMouseMoveHandler(wm_handle, uwin_on_move);
    WM_SetMouseReleaseHandler(wm_handle, uwin_on_release);

    /* Backing buffer is the client area size.
     * Client area = w - 1 (left border) - SB (right scrollbar) by
     *               h - TITLEBAR_H - SB (bottom scrollbar). */
    int buf_w = w - 1 - WM_SCROLLBAR_W;
    int buf_h = h - WM_TITLEBAR_H - WM_SCROLLBAR_W;
    if (buf_w < 32) buf_w = 32;
    if (buf_h < 32) buf_h = 32;

    uint32_t size = (uint32_t)buf_w * (uint32_t)buf_h * sizeof(uint32_t);
    uint32_t *buf = (uint32_t *)ELF64_HeapAlloc(size, 16);
    if (!buf) {
        WM_CloseWindow(wm_handle);
        return -1;
    }
    for (uint32_t i = 0; i < (uint32_t)buf_w * (uint32_t)buf_h; i++)
        buf[i] = WB_WHITE;

    UserWindow *u = &g_uwins[handle];
    u->active = 1;
    u->wm_handle = wm_handle;
    u->buf = buf;
    u->buf_w = buf_w;
    u->buf_h = buf_h;
    u->dirty = 1;
    u->last_scroll_x = 0;
    u->last_scroll_y = 0;
    u->ev_head = 0;
    u->ev_tail = 0;
    u->ev_count = 0;

    return handle;
}

int UserWindow_Destroy(int handle)
{
    if (handle < 0 || handle >= UWIN_MAX_WINDOWS || !g_uwins[handle].active)
        return -1;
    UserWindow *u = &g_uwins[handle];
    if (u->wm_handle >= 0)
        WM_CloseWindow(u->wm_handle);
    u->active = 0;
    u->wm_handle = -1;
    u->buf = NULL;
    u->buf_w = 0;
    u->buf_h = 0;
    return 0;
}

int UserWindow_SetScrollInfo(int handle, int content_w, int content_h)
{
    if (handle < 0 || handle >= UWIN_MAX_WINDOWS || !g_uwins[handle].active)
        return -1;
    UserWindow *u = &g_uwins[handle];
    WM_SetScrollInfo(u->wm_handle, content_w, content_h);
    return 0;
}

int UserWindow_SetScroll(int handle, int scroll_x, int scroll_y)
{
    if (handle < 0 || handle >= UWIN_MAX_WINDOWS || !g_uwins[handle].active)
        return -1;
    UserWindow *u = &g_uwins[handle];
    WM_SetScrollY(u->wm_handle, scroll_y);
    WM_SetScrollX(u->wm_handle, scroll_x);
    u->last_scroll_x = scroll_x;
    u->last_scroll_y = scroll_y;
    return 0;
}

int UserWindow_DrawText(int handle, int x, int y, const char *text, uint32_t color)
{
    if (handle < 0 || handle >= UWIN_MAX_WINDOWS || !g_uwins[handle].active || !text)
        return -1;
    UserWindow *u = &g_uwins[handle];
    buf_put_str(u, x, y, text, color, WB_WHITE);
    u->dirty = 1;
    return 0;
}

int UserWindow_DrawRect(int handle, int x, int y, int w, int h, uint32_t color)
{
    if (handle < 0 || handle >= UWIN_MAX_WINDOWS || !g_uwins[handle].active)
        return -1;
    UserWindow *u = &g_uwins[handle];
    if (w <= 0 || h <= 0)
        return 0;
    buf_fill_rect(u, x, y, w, h, color);
    u->dirty = 1;
    return 0;
}

int UserWindow_Present(int handle)
{
    if (handle < 0 || handle >= UWIN_MAX_WINDOWS || !g_uwins[handle].active)
        return -1;
    UserWindow *u = &g_uwins[handle];
    u->dirty = 1;
    WM_Redraw();
    return 0;
}

int UserWindow_GetEvent(int handle, struct uaos_gui_event *event)
{
    if (handle < 0 || handle >= UWIN_MAX_WINDOWS || !g_uwins[handle].active || !event)
        return -1;
    UserWindow *u = &g_uwins[handle];

    if (u->ev_count == 0)
        return 0;

    *event = u->events[u->ev_head];
    u->ev_head = (u->ev_head + 1) % UWIN_MAX_EVENT;
    u->ev_count--;
    return 1;
}
