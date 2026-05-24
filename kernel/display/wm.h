/* wm.h — UAOS Window Manager
 *
 * Manages a z-ordered stack of windows. Each window has a position, size,
 * title, and callbacks for draw and key input. The WM handles:
 *   - Click-to-focus (raises window to top of z-order)
 *   - Title-bar drag to move windows
 *   - Repainting windows back-to-front after move/focus change
 */

#ifndef UAOS_WM_H
#define UAOS_WM_H

#define WM_MAX_WINDOWS  8
#define WM_TITLEBAR_H   20
#define WM_BORDER       4    /* border thickness on left/right/bottom */
#define WM_SCROLLBAR_W  16   /* width of right scrollbar / height of bottom scrollbar */

typedef void (*WM_DrawFn)(int win_x, int win_y, int win_w, int win_h);
typedef void (*WM_KeyFn)(char c);
typedef void (*WM_ClickFn)(int win_handle, int mx, int my);

typedef struct {
    int        x, y, w, h;
    char       title[32];
    WM_DrawFn  draw;      /* called to repaint window contents at (x,y)   */
    WM_KeyFn   on_key;    /* called with keystrokes when window is focused */
    WM_ClickFn on_click;  /* called on client-area mouse press (may be 0)  */
    int        active;    /* 1 = registered, 0 = slot free                */
    /* Scroll state — set by window content via WM_SetScrollInfo */
    int        scroll_x;      /* current horizontal scroll offset (pixels) */
    int        scroll_y;      /* current vertical scroll offset (pixels)   */
    int        content_w;     /* total content width  (0 = same as client) */
    int        content_h;     /* total content height (0 = same as client) */
    /* Zoom / maximise state */
    int        zoomed;        /* 1 = currently maximised                   */
    int        restore_x, restore_y, restore_w, restore_h;
} WmWindow;

/* Register a window — returns handle (0..WM_MAX_WINDOWS-1) or -1 on fail */
int  WM_AddWindow(int x, int y, int w, int h, const char *title,
                  WM_DrawFn draw, WM_KeyFn on_key);

/* Set an optional client-area click callback on an existing window */
void WM_SetClickHandler(int handle, WM_ClickFn on_click);

/* Tell the WM the total content size so scrollbars can be proportional */
void WM_SetScrollInfo(int handle, int content_w, int content_h);

/* Query current scroll offsets */
int  WM_GetScrollX(int handle);
int  WM_GetScrollY(int handle);

/* Set vertical scroll offset (clamped to content range) */
void WM_SetScrollY(int handle, int y);

/* Call from main loop with current mouse state */
void WM_MouseEvent(int mx, int my, int btn_left);

/* Feed a keystroke to the focused window */
void WM_KeyEvent(char c);

/* Redraw all windows back-to-front, then cursor */
void WM_Redraw(void);

/* Get the currently focused window handle (-1 if none) */
int  WM_GetFocus(void);

/* Move a window to a new position and repaint */
void WM_MoveWindow(int handle, int new_x, int new_y);

/* Raise a window to the top of the z-order and give it focus */
void WM_RaiseWindow(int handle);

/* Close (destroy) a window by handle — repaints desktop */
void WM_CloseWindow(int handle);

/* Returns 1 if the handle refers to an active window */
int  WM_IsWindowActive(int handle);

#endif
