/*
 * audio.c — UAOS host audio mixer
 *
 * Mixes the four Paula audio channels into a single output and feeds a host
 * backend.  The current backend is the PC speaker, which can only produce a
 * square wave, so the mixer selects the loudest active channel and drives the
 * speaker with a frequency derived from its sample value.
 */

#include "audio/audio.h"
#include "chipset/chip_emu.h"
#include <stdint.h>

/* One-pole LED filter state per channel. */
static int16_t g_led_state[AUDIO_CHANNELS];

void audio_init(void)
{
    for (int i = 0; i < AUDIO_CHANNELS; i++) g_led_state[i] = 0;
    pc_speaker_tone(0);
}

int16_t audio_led_filter(int16_t in, int16_t *state)
{
    /* One-pole low-pass filter approximating the Amiga LED filter. */
    *state = (int16_t)((in + (31 * *state)) / 32);
    return *state;
}

void audio_tick(void)
{
    int16_t max_sample = 0;
    int max_ch = -1;

    for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
        uint16_t raw = chip_emu_audio_sample(ch);
        /* Convert unsigned 8-bit Paula sample to signed 16-bit. */
        int16_t s = (int16_t)(((int)(raw & 0xFF) - 128) * 256);
        s = audio_led_filter(s, &g_led_state[ch]);
        if (s < 0) s = (int16_t)-s;
        if (s > max_sample) {
            max_sample = s;
            max_ch = ch;
        }
    }

    if (max_ch < 0 || max_sample == 0) {
        pc_speaker_tone(0);
        return;
    }

    /* Drive the PC speaker with a frequency derived from the sample value. */
    uint32_t freq = (uint32_t)(max_sample / 16);
    if (freq < 30) freq = 30;
    if (freq > 12000) freq = 12000;
    pc_speaker_tone(freq);
}
