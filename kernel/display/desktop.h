/* desktop.h — UAOS Workbench-style Desktop */

#ifndef UAOS_DESKTOP_H
#define UAOS_DESKTOP_H

#include <stdint.h>
#include <stddef.h>

void Desktop_Draw(void);          /* render full desktop (call once after FB_Init) */
void Desktop_UpdateClock(void);   /* redraw clock area in menu bar                 */
void Desktop_RedrawRect(int rx, int ry, int rw, int rh); /* repaint backdrop rect */

/* Workbench load control - desktop only draws after LoadWB is called */
void Desktop_MarkWorkbenchLoaded(void);  /* call from LoadWB */
int  Desktop_IsWorkbenchLoaded(void);  /* check before drawing desktop */

/* Feed a mouse event to the desktop (icon hit-test / double-click / menu).
 * Call this from WM_MouseEvent when no window was hit.
 * left_pressed and right_pressed are 1 when the corresponding button was
 * just pressed this event.
 * Returns 1 if the desktop handled the event, 0 otherwise. */
int  Desktop_MouseEvent(int mx, int my, int left_pressed, int right_pressed);

/* Drag-tracking mouse events for desktop icons.
 * Call from WM_MouseEvent when no window drag/resize is active. */
void Desktop_MouseMove(int mx, int my, int btn_left);
void Desktop_MouseRelease(int mx, int my);
int  Desktop_IsDraggingIcon(void);

/* Hover tracking for desktop widgets (e.g. menu dropdown highlighting).
 * Call from WM_MouseEvent on mouse movement when no button is pressed. */
void Desktop_MouseHover(int mx, int my);

/* Right-button release: trigger the highlighted menu item and close the menu.
 * Call from WM_MouseEvent when the right mouse button is released. */
void Desktop_RightButtonRelease(int mx, int my);

/* Returns the current 1-Hz tick counter (incremented by Desktop_UpdateClock).
 * Used by the file browser for double-click timing. */
unsigned int Desktop_GetTick(void);

/* Set by Desktop_UpdateClock (from RTC IRQ) to request a WM_Redraw.
 * The main event loop calls Desktop_FlushClockRedraw() each iteration. */
void Desktop_FlushClockRedraw(void);

/* Screen title display — controlled by intuition.library ShowTitle() */
void Desktop_SetScreenTitle(const char *title, int show);

/* DisplayBeep flash — briefly tint the desktop backdrop with the given colour.
 * Pass 0 to clear the flash.  The caller is responsible for redrawing. */
void Desktop_DisplayBeepFlash(uint32_t color);

/* Active window menu helpers provided by intuition.library */
#define HOST_MENU_MAX      16
#define HOST_MENU_ITEM_MAX 32
#define HOST_MENU_LABEL_SIZE 64

typedef struct HostMenu HostMenu;

typedef struct {
    char label[HOST_MENU_LABEL_SIZE];
    int  enabled;
    int  has_checkmark; /* CHECKIT flag set (draw a check box) */
    int  checked;       /* CHECKIT flag currently set */
    int  toggle;        /* MENUTOGGLE flag set */
    char command_key;   /* COMMSEQ shortcut byte, 0 if none */
    int  has_submenu;
    HostMenu *submenu;  /* pointer to parsed sub-item list */
    uint32_t guest_item; /* guest MenuItem pointer for state updates */
} HostMenuItem;

struct HostMenu {
    char label[HOST_MENU_LABEL_SIZE];
    int  item_count;
    HostMenuItem items[HOST_MENU_ITEM_MAX];
};

uint32_t Intuition_GetActiveWindowMenuStrip(void);
void     Intuition_PostMenuPick(uint32_t menu_number);
int      Intuition_GetHostMenuStrip(uint32_t menu_strip, HostMenu *menus, int max_menus);
void     Intuition_UpdateMenuItemCheck(uint32_t guest_item, int toggle);

#endif
