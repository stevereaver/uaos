/* user_window.h — UAOS userspace GUI window syscall API
 *
 * Exposes a minimal windowing interface to native x86-64 tasks so they can
 * create a Workbench-style window, draw into its backing buffer, and poll for
 * input events.
 */

#ifndef UAOS_USER_WINDOW_H
#define UAOS_USER_WINDOW_H

#include <stdint.h>

#define UWIN_MAX_WINDOWS   4
#define UWIN_MAX_TITLE     32
#define UWIN_MAX_EVENT     16

#define UWIN_EVENT_NONE     0
#define UWIN_EVENT_KEY      1
#define UWIN_EVENT_CLICK    2
#define UWIN_EVENT_RELEASE  3
#define UWIN_EVENT_MOVE     4
#define UWIN_EVENT_SCROLL   5

struct uaos_gui_event {
    uint8_t  type;      /* UWIN_EVENT_* */
    uint8_t  button;    /* mouse button state for click/move/release */
    int16_t  x;         /* mouse x relative to client area, or key code */
    int16_t  y;         /* mouse y relative to client area, or scroll_y */
};

/* Called once from kernel main to register the user-window draw callbacks. */
void UserWindow_Init(void);

/* Syscall handlers — called from syscall_dispatch.c. */
int UserWindow_Create(const char *title, int x, int y, int w, int h);
int UserWindow_Destroy(int handle);
int UserWindow_SetScrollInfo(int handle, int content_w, int content_h);
int UserWindow_SetScroll(int handle, int scroll_x, int scroll_y);
int UserWindow_DrawText(int handle, int x, int y, const char *text, uint32_t color);
int UserWindow_DrawRect(int handle, int x, int y, int w, int h, uint32_t color);
int UserWindow_Present(int handle);
int UserWindow_GetEvent(int handle, struct uaos_gui_event *event);

/* Extended drawing primitives for widget toolkit */
int UserWindow_DrawLine(int handle, int x1, int y1, int x2, int y2, uint32_t color);
int UserWindow_FillRect(int handle, int x, int y, int w, int h, uint32_t color);
int UserWindow_Draw3DBorder(int handle, int x, int y, int w, int h, int raised, uint32_t base_color);
int UserWindow_DrawPixel(int handle, int x, int y, uint32_t color);
int UserWindow_DrawTextBg(int handle, int x, int y, const char *text, uint32_t fg, uint32_t bg);
int UserWindow_GetWinSize(int handle, int *w, int *h);
int UserWindow_SetTitle(int handle, const char *title);
int UserWindow_DrawEllipse(int handle, int cx, int cy, int rx, int ry, uint32_t color);

#endif /* UAOS_USER_WINDOW_H */
