/*
 * audio.h — UAOS host audio subsystem API
 *
 * Provides a minimal audio mixer and backend abstraction.  The mixer is
 * fed from the Paula emulator and drives a host backend (currently the
 * PC speaker as a simple square-wave output).
 */

#ifndef UAOS_AUDIO_H
#define UAOS_AUDIO_H

#include <stdint.h>

#define AUDIO_CHANNELS 4

/* Initialize the host audio backend.  Called once at boot. */
void audio_init(void);

/* Mix one Paula sample period and push the result to the host backend.
 * Called from the PIT tick path. */
void audio_tick(void);

/* Apply a simple one-pole LED-style low-pass filter to a sample. */
int16_t audio_led_filter(int16_t in, int16_t *state);

/* PC speaker backend: set the current output tone (frequency in Hz, 0 = off). */
void pc_speaker_tone(uint32_t freq_hz);

#endif /* UAOS_AUDIO_H */
