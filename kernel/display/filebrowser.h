/* filebrowser.h — UAOS Workbench-style file browser / drawer window */

#ifndef UAOS_FILEBROWSER_H
#define UAOS_FILEBROWSER_H

/* Open (or bring to front) a drawer window for the given volume.
 * volume: "RAM Disk" or "UAOS:"
 * Registers with the WM; safe to call multiple times (idempotent). */
void FileBrowser_Open(const char *volume);

#endif
