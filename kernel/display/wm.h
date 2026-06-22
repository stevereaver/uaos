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

#define WM_MAX_WINDOWS  16
#define WM_TITLEBAR_H   20
#define WM_BORDER       4    /* border thickness on left/right/bottom */
#define WM_SCROLLBAR_W  16   /* width of right scrollbar / height of bottom scrollbar */

typedef void (*WM_DrawFn)(int win_x, int win_y, int win_w, int win_h);
typedef void (*WM_KeyFn)(char c);
typedef void (*WM_ClickFn)(int win_handle, int mx, int my);
typedef void (*WM_MouseMoveFn)(int win_handle, int mx, int my);
typedef void (*WM_MouseReleaseFn)(int win_handle, int mx, int my);

typedef struct {
    int        x, y, w, h;
    char       title[32];
    WM_DrawFn  draw;      /* called to repaint window contents at (x,y)   */
    WM_KeyFn   on_key;    /* called with keystrokes when window is focused */
    WM_ClickFn on_click;  /* called on client-area mouse press (may be 0)  */
    WM_MouseMoveFn on_move;   /* called while mouse is held inside window   */
    WM_MouseReleaseFn on_release; /* called on mouse release inside window */
    int        active;    /* 1 = registered, 0 = slot free                */
    /* Scroll state — set by window content via WM_SetScrollInfo */
    int        scroll_x;      /* current horizontal scroll offset (pixels) */
    int        scroll_y;      /* current vertical scroll offset (pixels)   */
    int        content_w;     /* total content width  (0 = same as client) */
    int        content_h;     /* total content height (0 = same as client) */
    int        view_h;        /* visible viewport height set by content (0 = use client ch) */
    /* Zoom / maximise state */
    int        zoomed;        /* 1 = currently maximised                   */
    int        restore_x, restore_y, restore_w, restore_h;
} WmWindow;

/* Register a window — returns handle (0..WM_MAX_WINDOWS-1) or -1 on fail */
int  WM_AddWindow(int x, int y, int w, int h, const char *title,
                  WM_DrawFn draw, WM_KeyFn on_key);

/* Set optional client-area mouse callbacks on an existing window */
void WM_SetClickHandler(int handle, WM_ClickFn on_click);
void WM_SetMouseMoveHandler(int handle, WM_MouseMoveFn on_move);
void WM_SetMouseReleaseHandler(int handle, WM_MouseReleaseFn on_release);

/* Tell the WM the total content size and visible viewport height so scrollbars
 * can be proportional and scroll clamping is correct.
 * view_h: the actual visible content area height (may differ from client ch
 * if the window has internal chrome like an input bar). Pass 0 to use ch. */
void WM_SetScrollInfo(int handle, int content_w, int content_h);
void WM_SetScrollInfoEx(int handle, int content_w, int content_h, int view_h);

/* Query current scroll offsets */
int  WM_GetScrollX(int handle);
int  WM_GetScrollY(int handle);

/* Set scroll offsets (clamped to content range) */
void WM_SetScrollX(int handle, int x);
void WM_SetScrollY(int handle, int y);

/* Call from main loop with current mouse state.
 * btn_left and btn_right are 1 when the corresponding button is held. */
void WM_MouseEvent(int mx, int my, int btn_left, int btn_right);

/* Feed a keystroke to the focused window */
void WM_KeyEvent(char c);

/* Redraw all windows back-to-front, then cursor */
void WM_Redraw(void);

/* Get the currently focused window handle (-1 if none) */
int  WM_GetFocus(void);

/* Request focus for a window (raises it and gives focus) */
void WM_RequestFocus(int handle);

/* Move a window to a new position and repaint */
void WM_MoveWindow(int handle, int new_x, int new_y);

/* Raise a window to the top of the z-order and give it focus */
void WM_RaiseWindow(int handle);

/* Lower a window to the bottom of the z-order and redraw */
void WM_LowerWindow(int handle);

/* Request a repaint of a window's chrome (and full scene) */
void WM_RepaintWindow(int handle);

/* Set the title of an active window and repaint its chrome */
void WM_SetWindowTitle(int handle, const char *title);

/* Close (destroy) a window by handle — repaints desktop */
void WM_CloseWindow(int handle);

/* Returns 1 if the handle refers to an active window */
int       WM_IsWindowActive(int handle);
WM_DrawFn WM_GetDrawFn(int handle);   /* returns NULL if handle invalid/inactive */

/* Handle of the window currently being painted by WM_Redraw/repaint_window.
 * Draw callbacks can use this to identify themselves instead of hit-testing. */
extern int WM_CurrentDrawHandle;

/* Get the absolute screen geometry of an active window. Returns 1 if active. */
int       WM_GetWindowRect(int handle, int *x, int *y, int *w, int *h);

/* Copy the title of an active window into out (max bytes).  Returns 1 if active. */
int       WM_GetWindowTitle(int handle, char *out, int max);

#endif
