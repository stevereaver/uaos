/*
 * audio_test.c — UAOS audio subsystem tests
 *
 * Provides a simple sine-wave test and a minimal MOD-like pattern test to
 * exercise the Paula mixer and host backend.
 */

#include <stdint.h>
#include "audio/audio.h"

extern uint8_t *g_ram;

/* Paula DMA period for a given frequency (PAL Amiga clock). */
static uint16_t period_for_hz(uint32_t hz)
{
    if (hz == 0) return 0;
    return (uint16_t)(3546895UL / hz);
}

/* 8-bit unsigned sine wave, one cycle. */
static uint8_t g_sine_wave[64];

static void build_sine(void)
{
    static int built = 0;
    if (built) return;
    built = 1;
    for (int i = 0; i < 64; i++) {
        /* 127 * sin(2*pi*i/64) + 128 */
        static const int8_t sin64[64] = {
            0, 12, 25, 37, 49, 60, 71, 81, 90, 97, 104, 110, 115, 119, 122, 124,
            126, 124, 122, 119, 115, 110, 104, 97, 90, 81, 71, 60, 49, 37, 25, 12,
            0, -12, -25, -37, -49, -60, -71, -81, -90, -97, -104, -110, -115, -119, -122, -124,
            -126, -124, -122, -119, -115, -110, -104, -97, -90, -81, -71, -60, -49, -37, -25, -12
        };
        int v = (sin64[i] * 127) / 126 + 128;
        if (v > 255) v = 255;
        if (v < 0) v = 0;
        g_sine_wave[i] = (uint8_t)v;
    }
}

/* Play a ~1 kHz sine wave on Paula channel 0. */
void audio_sine_test(void)
{
    build_sine();

    uint32_t addr = 0x00010000u;
    for (int i = 0; i < 64; i++) g_ram[addr + i] = g_sine_wave[i];

    chip_emu_audio_set_channel(0, addr, 64 / 2, period_for_hz(1000), 48);
    chip_emu_audio_set_dmacon(0x0201u); /* master + channel 0 */
}

/* A minimal MOD-like pattern: four notes on channel 0. */
static const uint32_t g_pattern_freqs[4] = { 440, 554, 659, 880 };

void audio_pattern_test(void)
{
    build_sine();

    uint32_t addr = 0x00010100u;
    for (int i = 0; i < 64; i++) g_ram[addr + i] = g_sine_wave[i];

    /* Set the first note; a real MOD player would update AUD0PER from an
     * interrupt or task to sequence the pattern. */
    chip_emu_audio_set_channel(0, addr, 64 / 2, period_for_hz(g_pattern_freqs[0]), 48);
    chip_emu_audio_set_dmacon(0x0201u);
}
