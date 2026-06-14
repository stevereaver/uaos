/* vim_win.h — UAOS Vim Editor Window */

#ifndef UAOS_VIM_WIN_H
#define UAOS_VIM_WIN_H

#define VIM_MAX_SLOTS 4

/* Open a standalone vim window editing the given file.
 * Returns 0 on success, -1 if no free slot. */
int VimWin_Open(const char *filename);

/* Feed one keystroke to the focused vim window (called by main loop). */
void VimWin_HandleKey(char c);

/* Redraw all active vim windows (called after desktop repaint). */
void VimWin_Redraw(void);

/* Initialise vim config defaults (call once at boot). */
void VimWin_Init(void);

/* -------------------------------------------------------------------------
 * Inline (shell-integrated) vim mode
 * The editor takes over the calling shell window instead of opening a
 * separate WM window.  When the user quits the callback fires so the shell
 * can resume normal operation.
 * ------------------------------------------------------------------------- */

typedef void (*VimQuitFn)(void *shell_extra);

/* Open inline vim in a shell window.  Returns slot index (>=0) or -1.
 * Does NOT create a WM window — the shell draws and feeds keys directly. */
int VimWin_OpenInline(const char *filename, void *shell_extra, VimQuitFn on_quit);

/* Draw inline vim into the given shell window rect. */
void VimWin_DrawInline(int slot, int wx, int wy, int ww, int wh);

/* Feed a key to inline vim. */
void VimWin_KeyInline(int slot, char c);

/* Returns 1 if inline vim is still active. */
int VimWin_IsActive(int slot);

/* Get the filename of an active vim instance.  Returns 1 if active. */
int VimWin_GetFilename(int slot, char *out, int max);

/* Returns 1 if the vim instance at slot is in inline (shell-integrated) mode. */
int VimWin_IsInline(int slot);

#endif
