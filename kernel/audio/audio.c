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

/* Master output volume/mute — software gain applied in audio_ring_write,
 * plus the backend's codec-amp hook.  Default is full scale. */
static int g_volume = 80;
static int g_mute = 0;

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
    int vol = g_mute ? 0 : g_volume;
    for (unsigned int i = 0; i < n; i++) {
        unsigned int idx = g_ring_write * 2;
        int32_t l = src[i * 2 + 0], r = src[i * 2 + 1];
        if (vol != 100) {
            l = l * vol / 100;
            r = r * vol / 100;
        }
        g_ring[idx + 0] = (int16_t)l;
        g_ring[idx + 1] = (int16_t)r;
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
static uint32_t g_nz_count = 0, g_tot_count = 0;
uint32_t g_audio_nz_pct = 0; /* % of output samples that were nonzero (dump) */
static uint64_t g_last_audio_cycles = 0; /* last emulated cycle count */

/* Currently selected host backend. */
static AudioBackend *g_backend = NULL;

int16_t audio_led_filter(int16_t in, int16_t *state)
{
    /* One-pole low-pass approximating the Amiga's ~7 kHz LED filter.
     * The old (in + 31*state)/32 pole sat near 240 Hz and muffled all
     * treble (UAOS-366 "low/dull" audio). */
    *state = (int16_t)((in * 7 + (3 * *state)) / 10);
    return *state;
}

/* -------------------------------------------------------------------------
 * Master volume / mute
 * ------------------------------------------------------------------------- */

static void audio_apply_volume(void)
{
    if (g_backend && g_backend->set_volume)
        g_backend->set_volume(g_volume, g_mute);
}

void audio_set_volume(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_volume = pct;
    audio_apply_volume();
}

int audio_get_volume(void) { return g_volume; }

void audio_set_mute(int on)
{
    g_mute = !!on;
    audio_apply_volume();
}

int audio_get_mute(void) { return g_mute; }

void audio_mute_toggle(void) { audio_set_mute(!g_mute); }

void audio_adjust_volume(int delta_pct)
{
    audio_set_volume(g_volume + delta_pct);
}

/* -------------------------------------------------------------------------
 * Backend selection
 * ------------------------------------------------------------------------- */

static void audio_select_backend(void)
{
    if (audio_backend_hda.init()) {
        g_backend = &audio_backend_hda;
        return;
    }
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

/* -------------------------------------------------------------------------
 * Error beep — square-wave burst mixed into the generated stream.
 * ------------------------------------------------------------------------- */

#define BEEP_AMPLITUDE 8192   /* quarter scale, added to both channels */

static uint32_t g_beep_step  = 0;  /* 16.16 phase step per sample */
static uint32_t g_beep_phase = 0;
static int      g_beep_left  = 0;  /* frames remaining */

void audio_beep(uint32_t freq_hz, uint32_t ms)
{
    if (freq_hz == 0 || ms == 0) { g_beep_left = 0; return; }
    g_beep_step  = (uint32_t)(((uint64_t)freq_hz * 65536ULL) / AUDIO_SAMPLE_RATE);
    g_beep_phase = 0;
    g_beep_left  = (int)((AUDIO_SAMPLE_RATE * ms) / 1000u);
}

static int16_t beep_sample(void)
{
    if (g_beep_left <= 0) return 0;
    g_beep_left--;
    g_beep_phase += g_beep_step;
    return (g_beep_phase & 0x8000u) ? BEEP_AMPLITUDE : -BEEP_AMPLITUDE;
}

void audio_init(void)
{
    for (int i = 0; i < AUDIO_CHANNELS; i++) g_led_state[i] = 0;
    g_amiga_clock_frac = 0;
    g_ring_write = 0;
    g_ring_read = 0;
    g_ring_count = 0;
    g_backend = NULL;

    audio_select_backend();
    audio_apply_volume();       /* push default 50/unmuted to the codec */
}

const char *audio_backend_name(void)
{
    return g_backend ? g_backend->name : NULL;
}

/* Generate up to n frames of 48 kHz stereo samples into g_mix_buf
 * (g_mix_buf holds SAMPLES_PER_TICK, so callers pass n <= that). */
static unsigned int audio_generate(unsigned int n)
{
    for (unsigned int i = 0; i < n; i++) {
        /* Advance Paula DMA at the fixed real Amiga-clock rate per output
         * sample — correct sample pitch regardless of guest speed.  Pacing
         * it off the emulated cycle counter made it chase the (too-fast)
         * guest -> chipmunk pitch (UAOS-366).  The scheduler-side per-line
         * advance is removed, so this is the only advance (no double). */
        g_amiga_clock_frac += AMIGA_DELTA16;
        uint32_t cycles = g_amiga_clock_frac >> 16;
        g_amiga_clock_frac &= 0xFFFFu;
        chip_emu_audio_advance(cycles);

        int32_t mix_l = 0, mix_r = 0;
        for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
            /* Paula samples are SIGNED 8-bit; chip_emu_audio_sample_8bit
             * returns the value already signed — scale it, don't re-center
             * (the old (uint8_t)ret-128 folded every sample to |v|-128 ->
             * rectified, crackly, quiet — UAOS-366). */
            int16_t s = (int16_t)chip_emu_audio_sample_8bit(ch) * 256;
            s = audio_led_filter(s, &g_led_state[ch]);

            /* Apply channel volume (0-64), halved for two channels per
             * side so the sum cannot clip (UAOS-366 crackle). */
            uint8_t vol = chip_emu_audio_volume(ch);
            if (vol) {
                s = (int16_t)((int32_t)s * (int32_t)vol / 128);
            } else {
                s = 0;
            }

            /* Pan: channels 0/2 -> left, 1/3 -> right. */
            if (ch & 1u) mix_r += s;
            else         mix_l += s;
        }

        /* Error beep rides on top of the Paula mix. */
        int16_t b = beep_sample();
        if (b) { mix_l += b; mix_r += b; }

        /* Clamp to 16-bit signed range. */
        if (mix_l > 32767)  mix_l = 32767;
        if (mix_l < -32768) mix_l = -32768;
        if (mix_r > 32767)  mix_r = 32767;
        if (mix_r < -32768) mix_r = -32768;

        g_mix_buf[i * 2 + 0] = (int16_t)mix_l;
        g_mix_buf[i * 2 + 1] = (int16_t)mix_r;
        g_nz_count += (mix_l || mix_r) ? 1u : 0u;
        g_tot_count++;
        if (g_tot_count >= AUDIO_SAMPLE_RATE) {
            g_audio_nz_pct = (unsigned int)((g_nz_count * 100ull) / g_tot_count);
            g_nz_count = 0; g_tot_count = 0;
        }
    }
    return n;
}

static int g_ring_solo = 0;

void audio_ring_solo(int on)
{
    g_ring_solo = on;
}

void audio_tick(void)
{
    if (!g_ring_solo) {
        /* Keep the ring ~half full — produce whatever the backend drained
         * since the last tick instead of a fixed 480 frames.  Production
         * now tracks the codec's real consumption, so tick jitter or a
         * backend draining at a slightly different rate can't creep the
         * ring to empty (silence) or full (dropped tail) — the source of
         * the "dropping in and out" chop on HDA (UAOS-366). */
        unsigned int guard = 0;
        while (audio_ring_ready() < AUDIO_RING_TARGET &&
               guard++ < (AUDIO_RING_SAMPLES / SAMPLES_PER_TICK)) {
            unsigned int want = AUDIO_RING_TARGET - audio_ring_ready();
            if (want > SAMPLES_PER_TICK) want = SAMPLES_PER_TICK;
            audio_ring_write(g_mix_buf, audio_generate(want));
        }
    }

    if (g_backend && g_backend->service) {
        g_backend->service();
    }
}
