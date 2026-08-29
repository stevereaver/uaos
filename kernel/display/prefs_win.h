/* prefs_win.h — UAOS Preferences Editors
 *
 * GUI editors for AmigaOS 3.x-style preferences, launched from
 * the Prefs: programs (ScreenMode, Font, IControl, Input, Palette,
 * WBPattern, Serial, Printer, Time, Locale).
 */

#ifndef UAOS_PREFS_WIN_H
#define UAOS_PREFS_WIN_H

#include <stdint.h>

/* Individual editor show/hide — each opens a WM window */
void PalettePrefs_Show(void);
void TimePrefs_Show(void);
void IControlPrefs_Show(void);
void InputPrefs_Show(void);
void ScreenModePrefs_Show(void);
void WBPatternPrefs_Show(void);
void FontPrefs_Show(void);
void SerialPrefs_Show(void);
void PrinterPrefs_Show(void);
void LocalePrefs_Show(void);

/* Check if any prefs window is open */
int PrefsWin_AnyOpen(void);

#endif /* UAOS_PREFS_WIN_H */
