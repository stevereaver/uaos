/* desktop.h — UAOS Workbench-style Desktop */

#ifndef UAOS_DESKTOP_H
#define UAOS_DESKTOP_H

void Desktop_Draw(void);          /* render full desktop (call once after FB_Init) */
void Desktop_UpdateClock(void);   /* redraw clock area in menu bar                 */
void Desktop_RedrawRect(int rx, int ry, int rw, int rh); /* repaint backdrop rect */

/* Workbench load control - desktop only draws after LoadWB is called */
void Desktop_MarkWorkbenchLoaded(void);  /* call from LoadWB */
int  Desktop_IsWorkbenchLoaded(void);  /* check before drawing desktop */

/* Feed a mouse event to the desktop (icon hit-test / double-click).
 * Call this from WM_MouseEvent when no window was hit.
 * Returns 1 if the desktop handled the event, 0 otherwise. */
int  Desktop_MouseEvent(int mx, int my, int btn_pressed);

/* Drag-tracking mouse events for desktop icons.
 * Call from WM_MouseEvent when no window drag/resize is active. */
void Desktop_MouseMove(int mx, int my, int btn_left);
void Desktop_MouseRelease(int mx, int my);
int  Desktop_IsDraggingIcon(void);

/* Returns the current 1-Hz tick counter (incremented by Desktop_UpdateClock).
 * Used by the file browser for double-click timing. */
unsigned int Desktop_GetTick(void);

/* Set by Desktop_UpdateClock (from RTC IRQ) to request a WM_Redraw.
 * The main event loop calls Desktop_FlushClockRedraw() each iteration. */
void Desktop_FlushClockRedraw(void);

#endif
