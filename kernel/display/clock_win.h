/*
 * clock_win.h — UAOS Clock Application
 *
 * Workbench-style clock window showing current date, local time, and
 * timezone.  Updates automatically once per second via ClockWin_Tick().
 */
#ifndef UAOS_CLOCK_WIN_H
#define UAOS_CLOCK_WIN_H

/* Open (or raise) the clock window */
void ClockWin_Open(void);

/* Called once per second from Desktop_UpdateClock to refresh the display */
void ClockWin_Tick(void);

#endif /* UAOS_CLOCK_WIN_H */
