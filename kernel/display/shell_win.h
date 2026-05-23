/* shell_win.h — UAOS Framebuffer Shell Window */

#ifndef UAOS_SHELL_WIN_H
#define UAOS_SHELL_WIN_H

/* Draw the initial shell window on the desktop */
void ShellWin_Init(void);

/* Feed one ASCII character into the shell (from keyboard IRQ) */
void ShellWin_HandleKey(char c);

/* Redraw the shell window contents (call after desktop repaint) */
void ShellWin_Redraw(void);

#endif
