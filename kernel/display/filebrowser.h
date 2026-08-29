/* filebrowser.h — UAOS Workbench-style file browser / drawer window */

#ifndef UAOS_FILEBROWSER_H
#define UAOS_FILEBROWSER_H

/* View modes for the Window ▸ View By flyout. */
typedef enum {
    VIEW_ICON = 0,   /* default icon grid */
    VIEW_NAME,       /* list sorted by name */
    VIEW_DATE,       /* list sorted by mtime (newest first) */
    VIEW_SIZE,       /* list sorted by size (largest first) */
    VIEW_TYPE,       /* list sorted by type then name */
    VIEW_MODE_COUNT
} ViewMode;

/* Open (or bring to front) a drawer window for the given volume.
 * volume: "RAM Disk" or "UAOS:"
 * Registers with the WM; safe to call multiple times (idempotent). */
void FileBrowser_Open(const char *volume);

/* Cancel pending double-click state in all browser slots except the one
 * owning wm_handle_keep.  Pass -1 to cancel all.  Call on every btn_pressed
 * that hits a window so stale first-clicks in background browsers cannot
 * complete as spurious double-clicks later. */
void FileBrowser_CancelClicks(int wm_handle_keep);

/* Return the volume/path of the browser that owns the focused WM window.
 * Returns NULL if no browser is focused. */
const char *FileBrowser_GetFocusedPath(void);

/* Return the WM handle of the focused browser, or -1 if none. */
int FileBrowser_GetFocusedHandle(void);

/* Get the name of the first selected entry in the focused browser.
 * Returns NULL if no browser focused or no entry selected. */
const char *FileBrowser_GetSelectedName(void);

/* Build the full path of the first selected entry in the focused browser.
 * Writes to dst[max]. Returns 1 on success, 0 on failure. */
int FileBrowser_GetSelectedPath(char *dst, int max);

/* Refresh entries for the browser owning wm_handle (re-read from VFS). */
void FileBrowser_Refresh(int wm_handle);

/* Select all entries in the browser owning wm_handle. */
void FileBrowser_SelectAll(int wm_handle);

/* Close the browser owning wm_handle. */
void FileBrowser_Close(int wm_handle);

/* Clean Up: re-arrange icons in a grid for the browser owning wm_handle. */
void FileBrowser_CleanUp(int wm_handle);

/* Snapshot the focused browser window's position/size so it is restored
 * when the same volume is reopened.  In-memory only (live CD). */
void FileBrowser_Snapshot(int wm_handle);

/* Set/get the view mode (View By flyout) for the browser owning wm_handle. */
void FileBrowser_SetViewMode(int wm_handle, ViewMode mode);
ViewMode FileBrowser_GetViewMode(int wm_handle);

/* Snapshot the selected icon's current position (Icons ▸ Snapshot). */
void FileBrowser_SnapshotIcon(int wm_handle);

/* Clear the saved position of the selected icon (Icons ▸ Unsnapshot). */
void FileBrowser_UnsnapshotIcon(int wm_handle);

#endif
