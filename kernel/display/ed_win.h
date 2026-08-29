/* ed_win.h — UAOS ED Editor (AmigaED-style line editor) */

#ifndef UAOS_ED_WIN_H
#define UAOS_ED_WIN_H

/* Open an ED editor window editing the given file.
 * Returns 0 on success, -1 on failure. */
int EdWin_Open(const char *filename);

/* Feed a keystroke to the focused ED window. */
void EdWin_HandleKey(char c);

/* Redraw all active ED windows. */
void EdWin_Redraw(void);

/* Initialize ED defaults (call once). */
void EdWin_Init(void);

/* ----- Inline (shell-integrated) mode ----- */
typedef void (*EdQuitFn)(void *shell_extra);

int EdWin_OpenInline(const char *filename, void *shell_extra, EdQuitFn on_quit);
void EdWin_DrawInline(int slot, int wx, int wy, int ww, int wh);
void EdWin_KeyInline(int slot, char c);
int EdWin_IsActive(int slot);
int EdWin_GetFilename(int slot, char *out, int max);
int EdWin_IsInline(int slot);

#endif
