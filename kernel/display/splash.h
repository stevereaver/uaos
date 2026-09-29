/* splash.h — UAOS Boot Splash Screen
 *
 * Draws the embedded splash artwork onto the linear framebuffer during
 * kernel init (pre-scheduler, pre-WM).  The image is converted at build
 * time by tools/make_splash.py and linked in as a binary blob
 * (ld -r -b binary).  Safe to call before any WM/scheduler setup — the
 * FB primitives write straight to VRAM in direct mode.
 */

#ifndef UAOS_SPLASH_H
#define UAOS_SPLASH_H

/* Paint the splash: fills the screen with the image's border colour and
 * blits the artwork centred (centre-cropped if larger than the mode).
 * No-op if there is no framebuffer or the blob is corrupt. */
void Splash_Show(void);

/* Brief busy-wait so the splash is actually visible.  Pre-PIT safe:
 * uses a pause spin loop, not g_pit_ticks. */
void Splash_Dwell(void);

#endif /* UAOS_SPLASH_H */
