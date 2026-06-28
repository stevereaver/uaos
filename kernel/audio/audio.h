/*
 * audio.h — UAOS host audio subsystem API
 *
 * Provides a 48 kHz stereo mixer fed by the Paula emulator and a pluggable
 * host backend.  Backends are selected at boot: AC97 is preferred, with the PC
 * speaker as a fallback.
 */

#ifndef UAOS_AUDIO_H
#define UAOS_AUDIO_H

#include <stdint.h>

#define AUDIO_CHANNELS    4
#define AUDIO_SAMPLE_RATE 48000

/* -------------------------------------------------------------------------
 * Host audio backend interface
 * ------------------------------------------------------------------------- */

typedef struct AudioBackend {
    /* One-time init; returns 1 on success, 0 if the hardware is absent. */
    int (*init)(void);

    /* Called from the 100 Hz PIT tick to push mixed samples to the host.
     * The backend reads the ring buffer produced by the mixer. */
    void (*service)(void);

    /* Shut the backend down (optional, may be NULL). */
    void (*shutdown)(void);

    /* Human-readable name for diagnostics. */
    const char *name;
} AudioBackend;

/* -------------------------------------------------------------------------
 * Mixer API
 * ------------------------------------------------------------------------- */

/* Initialize the host audio backend.  Called once at boot. */
void audio_init(void);

/* Return the human-readable name of the active backend, or NULL if none. */
const char *audio_backend_name(void);

/* Service the audio mixer and backend.  Called from the PIT tick path. */
void audio_tick(void);

/* Apply a simple one-pole LED-style low-pass filter to a sample. */
int16_t audio_led_filter(int16_t in, int16_t *state);

/* -------------------------------------------------------------------------
 * Ring buffer (producer/consumer between mixer and active backend)
 * ------------------------------------------------------------------------- */

#define AUDIO_RING_SAMPLES 16384

/* Return the number of free stereo frames the mixer can write. */
unsigned int audio_ring_free(void);

/* Return the number of stereo frames available for the backend to read. */
unsigned int audio_ring_ready(void);

/* Write `n` stereo frames from `src` into the ring buffer.  Returns the
 * number of frames actually written. */
unsigned int audio_ring_write(const int16_t *src, unsigned int n);

/* Read up to `n` stereo frames from the ring buffer into `dst`.  Returns the
 * number of frames actually read. */
unsigned int audio_ring_read(int16_t *dst, unsigned int n);

/* -------------------------------------------------------------------------
 * Paula→host helpers exposed to the mixer
 * ------------------------------------------------------------------------- */

/* Advance Paula audio DMA by the given number of Amiga master-clock cycles.
 * Defined in the chipset emulator. */
extern void chip_emu_audio_advance(uint32_t amiga_cycles);

/* Return the current 8-bit signed sample for the given Paula channel. */
extern int8_t chip_emu_audio_sample_8bit(int ch);

/* Return the current volume (0-64) for the given Paula channel. */
extern uint8_t chip_emu_audio_volume(int ch);

/* Directly configure a Paula channel / DMA control word (used by tests that
 * run before the chip-window page fault handler is installed). */
extern void chip_emu_audio_set_channel(int ch, uint32_t ptr, uint16_t len, uint16_t per, uint16_t vol);
extern void chip_emu_audio_set_dmacon(uint16_t dmacon);

/* -------------------------------------------------------------------------
 * Backend implementations
 * ------------------------------------------------------------------------- */

extern AudioBackend audio_backend_ac97;
extern AudioBackend audio_backend_pcspk;

#endif /* UAOS_AUDIO_H */
