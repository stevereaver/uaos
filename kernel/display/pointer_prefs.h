/* pointer_prefs.h — UAOS Pointer Preferences Tool
 *
 * AmigaOS 3.1-style pointer preferences editor.
 * Allows customization of cursor size, colors, and visibility options.
 */

#ifndef UAOS_POINTER_PREFS_H
#define UAOS_POINTER_PREFS_H

#include <stdint.h>

/* Initialize and show the pointer preferences window */
void PointerPrefs_Show(void);

/* Hide the pointer preferences window */
void PointerPrefs_Hide(void);

/* Check if pointer prefs window is currently open */
int PointerPrefs_IsOpen(void);

#endif
