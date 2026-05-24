/* shell_win.h — UAOS Framebuffer Shell Window */

#ifndef UAOS_SHELL_WIN_H
#define UAOS_SHELL_WIN_H

/* Open the first shell window at boot */
void ShellWin_Init(void);

/* Open a new independent shell window (up to MAX_SHELLS) */
void ShellWin_Open(void);

/* Feed one ASCII character into the focused shell (from keyboard IRQ) */
void ShellWin_HandleKey(char c);

/* Redraw all shell windows (call after desktop repaint) */
void ShellWin_Redraw(void);

#endif
