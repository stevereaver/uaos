/*
 * audio.c — UAOS host audio mixer
 *
 * Mixes the four Paula audio channels into a 48 kHz stereo ring buffer and
 * drives a pluggable host backend (AC97 preferred, PC speaker fallback).
 *
 * The mixer is called from the 100 Hz PIT tick.  Each tick it generates enough
 * 48 kHz samples to match the host sample rate (≈480 samples/tick) and writes
 * them to a lock-free ring buffer.  The active backend services the ring buffer
 * in the same tick, copying samples to its hardware DMA buffer or updating the
 * PC speaker.
 */

#include "audio/audio.h"
#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Paula→Amiga clock constants
 * ------------------------------------------------------------------------- */

/* PAL Amiga master clock (Paula DMA rate) ≈ 3.546895 MHz */
#define AMIGA_CLOCK_PAL 3546895UL

/* 16.16 fixed-point delta of Amiga clock cycles per 48 kHz sample. */
#define AMIGA_DELTA16 ((uint32_t)(((uint64_t)AMIGA_CLOCK_PAL * 65536ULL) / AUDIO_SAMPLE_RATE))

/* -------------------------------------------------------------------------
 * Ring buffer
 * ------------------------------------------------------------------------- */

static int16_t g_ring[AUDIO_RING_SAMPLES * 2]; /* interleaved L/R */
static unsigned int g_ring_write = 0;
static unsigned int g_ring_read = 0;
static unsigned int g_ring_count = 0;

unsigned int audio_ring_free(void)
{
    return AUDIO_RING_SAMPLES - g_ring_count;
}

unsigned int audio_ring_ready(void)
{
    return g_ring_count;
}

unsigned int audio_ring_write(const int16_t *src, unsigned int n)
{
    if (n > audio_ring_free()) n = audio_ring_free();
    for (unsigned int i = 0; i < n; i++) {
        unsigned int idx = g_ring_write * 2;
        g_ring[idx + 0] = src[i * 2 + 0];
        g_ring[idx + 1] = src[i * 2 + 1];
        g_ring_write = (g_ring_write + 1) & (AUDIO_RING_SAMPLES - 1);
        g_ring_count++;
    }
    return n;
}

unsigned int audio_ring_read(int16_t *dst, unsigned int n)
{
    if (n > audio_ring_ready()) n = audio_ring_ready();
    for (unsigned int i = 0; i < n; i++) {
        unsigned int idx = g_ring_read * 2;
        dst[i * 2 + 0] = g_ring[idx + 0];
        dst[i * 2 + 1] = g_ring[idx + 1];
        g_ring_read = (g_ring_read + 1) & (AUDIO_RING_SAMPLES - 1);
        g_ring_count--;
    }
    return n;
}

/* -------------------------------------------------------------------------
 * Mixer state
 * ------------------------------------------------------------------------- */

static int16_t g_led_state[AUDIO_CHANNELS];
static uint32_t g_amiga_clock_frac = 0; /* 16.16 fractional accumulator */

/* Currently selected host backend. */
static AudioBackend *g_backend = NULL;

int16_t audio_led_filter(int16_t in, int16_t *state)
{
    /* One-pole low-pass filter approximating the Amiga LED filter. */
    *state = (int16_t)((in + (31 * *state)) / 32);
    return *state;
}

/* -------------------------------------------------------------------------
 * Backend selection
 * ------------------------------------------------------------------------- */

static void audio_select_backend(void)
{
    if (audio_backend_ac97.init()) {
        g_backend = &audio_backend_ac97;
        return;
    }
    if (audio_backend_pcspk.init()) {
        g_backend = &audio_backend_pcspk;
        return;
    }
    /* No backend available; mixer will still run but samples are discarded. */
    g_backend = NULL;
}

/* -------------------------------------------------------------------------
 * Mixer
 * ------------------------------------------------------------------------- */

/* Number of 48 kHz stereo frames to produce per 100 Hz PIT tick. */
#define SAMPLES_PER_TICK (AUDIO_SAMPLE_RATE / 100)

/* Small local mix buffer. */
static int16_t g_mix_buf[SAMPLES_PER_TICK * 2];

void audio_init(void)
{
    for (int i = 0; i < AUDIO_CHANNELS; i++) g_led_state[i] = 0;
    g_amiga_clock_frac = 0;
    g_ring_write = 0;
    g_ring_read = 0;
    g_ring_count = 0;
    g_backend = NULL;

    audio_select_backend();
}

const char *audio_backend_name(void)
{
    return g_backend ? g_backend->name : NULL;
}

/* Generate a batch of 48 kHz stereo samples into g_mix_buf. */
static unsigned int audio_generate(void)
{
    for (int i = 0; i < SAMPLES_PER_TICK; i++) {
        /* Advance Paula DMA by the integer Amiga cycles for this sample. */
        g_amiga_clock_frac += AMIGA_DELTA16;
        uint32_t cycles = g_amiga_clock_frac >> 16;
        g_amiga_clock_frac &= 0xFFFFu;
        chip_emu_audio_advance(cycles);

        int32_t mix_l = 0, mix_r = 0;
        for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
            /* Paula samples are 8-bit unsigned; convert to signed 16-bit. */
            int16_t s = (int16_t)(((int)(uint8_t)chip_emu_audio_sample_8bit(ch) - 128) * 256);
            s = audio_led_filter(s, &g_led_state[ch]);

            /* Apply channel volume (0-64). */
            uint8_t vol = chip_emu_audio_volume(ch);
            if (vol) {
                s = (int16_t)((int32_t)s * (int32_t)vol / 64);
            } else {
                s = 0;
            }

            /* Pan: channels 0/2 -> left, 1/3 -> right. */
            if (ch & 1u) mix_r += s;
            else         mix_l += s;
        }

        /* Clamp to 16-bit signed range. */
        if (mix_l > 32767)  mix_l = 32767;
        if (mix_l < -32768) mix_l = -32768;
        if (mix_r > 32767)  mix_r = 32767;
        if (mix_r < -32768) mix_r = -32768;

        g_mix_buf[i * 2 + 0] = (int16_t)mix_l;
        g_mix_buf[i * 2 + 1] = (int16_t)mix_r;
    }
    return SAMPLES_PER_TICK;
}

void audio_tick(void)
{
    unsigned int produced = audio_generate();
    audio_ring_write(g_mix_buf, produced);

    if (g_backend && g_backend->service) {
        g_backend->service();
    }
}
