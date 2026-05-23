/* desktop.h — UAOS Workbench-style Desktop */

#ifndef UAOS_DESKTOP_H
#define UAOS_DESKTOP_H

void Desktop_Draw(void);          /* render full desktop (call once after FB_Init) */
void Desktop_UpdateClock(void);   /* redraw clock area in menu bar                 */
void Desktop_RedrawRect(int rx, int ry, int rw, int rh); /* repaint backdrop rect */

#endif
