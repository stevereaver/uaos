/* early_startup.h — UAOS Early Startup Control */

#ifndef UAOS_EARLY_STARTUP_H
#define UAOS_EARLY_STARTUP_H

/* Run the Early Startup Control screen.
 * Returns: 0 = normal boot, 1 = skip startup sequence, 2 = shell only (no WB)
 * Must be called after FB_Init and PS/2 keyboard init, before ShellWin_Init.
 * Displays a countdown — if no key is pressed within the timeout, returns 0.
 */
int EarlyStartup_Run(void);

#endif
