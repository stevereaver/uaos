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

/* Execute S:Startup-Sequence in the first shell instance */
void ShellWin_RunStartupSequence(void);

/* Poll background job queue — call from the main event loop.
 * Runs one queued job to completion (commands that yield will
 * still pump UI/network during their execution). */
void ShellWin_PollJobs(void);

/* List background jobs for the given shell.
 * The print callback receives (shell_opaque, line_text). */
void ShellWin_ListJobs(void *shell, void (*print)(void *, const char *));

/* Dispatch a single command line through the first shell instance.
 * Safe to call from any task after ShellWin_Init() has run. */
void ShellWin_DispatchLine(const char *line);

#endif
