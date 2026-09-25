/* shell_win.h — UAOS Framebuffer Shell Window */

#ifndef UAOS_SHELL_WIN_H
#define UAOS_SHELL_WIN_H

/* Open the first shell window at boot */
void ShellWin_Init(void);

/* Open a new independent shell window (up to MAX_SHELLS) */
void ShellWin_Open(void);

/* Open a new shell window and optionally execute a startup script in it.
 * The script path must be an absolute VFS path (e.g. resolved by the
 * caller relative to the invoking shell's cwd).  Pass NULL to just open
 * a shell with no startup script — equivalent to ShellWin_Open(). */
void ShellWin_OpenWithScript(const char *script_path);

/* Feed one ASCII character into the focused shell (from keyboard IRQ) */
void ShellWin_HandleKey(char c);

/* Redraw all shell windows (call after desktop repaint) */
void ShellWin_Redraw(void);

/* Execute S:Startup-Sequence in the first shell instance */
void ShellWin_RunStartupSequence(void);

/* Set shell-only mode (disables LoadWB — for Early Startup Control) */
void ShellWin_SetShellOnlyMode(int mode);

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

/* -------------------------------------------------------------------------
 * Remote (telnet) shell sessions
 *
 * A remote session is a ShellInstance with no WM window: output is sent
 * over an already-connected TCP socket and input arrives via
 * ShellWin_RemoteFeed().  The caller (telnetd) owns the socket and the
 * connection lifecycle; the shell owns the session slot and its task.
 * ------------------------------------------------------------------------- */

/* Virtual key codes accepted by ShellWin_RemoteFeed for non-ASCII input
 * (same codes the local line editor receives for cursor keys). */
#define SHELL_VKEY_UP    0x03
#define SHELL_VKEY_DOWN  0x04
#define SHELL_VKEY_LEFT  0x05
#define SHELL_VKEY_RIGHT 0x06

/* Open a remote shell session bound to an accepted TCP socket index.
 * Sends the banner and prompt immediately, then spawns the session task.
 * Returns an opaque session handle, or NULL if no remote slot is free. */
void *ShellWin_RemoteOpen(int tcp_sock);

/* Feed one character into the session's input queue (NVT-decoded by the
 * caller: printable ASCII, '\r', '\b', '\t', or SHELL_VKEY_*). */
void  ShellWin_RemoteFeed(void *session, char c);

/* Returns 1 once the session slot has been released (disconnect handled,
 * endcli, or task exit) — the caller should close the socket then. */
int   ShellWin_RemoteIsDead(void *session);

/* Signal the session to terminate (peer went away).  The session task
 * exits and releases its slot; RemoteIsDead then reports 1. */
void  ShellWin_RemoteKill(void *session);

#endif
