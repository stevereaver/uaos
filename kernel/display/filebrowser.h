/* filebrowser.h — UAOS Workbench-style file browser / drawer window */

#ifndef UAOS_FILEBROWSER_H
#define UAOS_FILEBROWSER_H

/* Open (or bring to front) a drawer window for the given volume.
 * volume: "RAM Disk" or "UAOS:"
 * Registers with the WM; safe to call multiple times (idempotent). */
void FileBrowser_Open(const char *volume);

/* Cancel pending double-click state in all browser slots except the one
 * owning wm_handle_keep.  Pass -1 to cancel all.  Call on every btn_pressed
 * that hits a window so stale first-clicks in background browsers cannot
 * complete as spurious double-clicks later. */
void FileBrowser_CancelClicks(int wm_handle_keep);

#endif
