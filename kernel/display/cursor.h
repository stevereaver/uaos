/* cursor.h — UAOS Framebuffer Hardware Cursor */

#ifndef UAOS_CURSOR_H
#define UAOS_CURSOR_H

/* Initialise cursor state and draw at starting position */
void Cursor_Init(int x, int y);

/* Move cursor to (x, y): restore background, draw at new position */
void Cursor_Move(int x, int y);

/* Draw cursor at current position (call after desktop redraw) */
void Cursor_Redraw(void);

/* Remove cursor from screen (restore background) — call before any repaint */
void Cursor_Hide(void);

#endif
