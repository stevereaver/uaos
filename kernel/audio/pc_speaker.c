/*
 * pc_speaker.c — UAOS PC speaker audio backend
 *
 * Fallback backend that consumes the 48 kHz stereo ring buffer and drives the
 * legacy PC speaker (PIT channel 2 + gate port 0x61).  The speaker is updated at
 * the 100 Hz PIT rate with a tone derived from the average sample energy.
 */

#include "audio/pc_speaker.h"
#include <stdint.h>

#define PIT_DATA2   0x42
#define PIT_CMD     0x43
#define SPEAKER_CTL 0x61

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

void pc_speaker_tone(uint32_t freq_hz)
{
    if (freq_hz == 0) {
        uint8_t ctl = inb(SPEAKER_CTL);
        outb(SPEAKER_CTL, (uint8_t)(ctl & ~0x03u));
        return;
    }

    uint32_t divisor = 1193182u / freq_hz;
    if (divisor > 0xFFFFu) divisor = 0xFFFFu;
    if (divisor < 1) divisor = 1;

    outb(PIT_CMD, 0xB6u);
    outb(PIT_DATA2, (uint8_t)(divisor & 0xFFu));
    outb(PIT_DATA2, (uint8_t)((divisor >> 8) & 0xFFu));

    uint8_t ctl = inb(SPEAKER_CTL);
    outb(SPEAKER_CTL, (uint8_t)(ctl | 0x03u));
}

#define PCSPK_BATCH 480

static int16_t g_pcspk_buf[PCSPK_BATCH * 2];

static int pcspk_backend_init(void)
{
    pc_speaker_tone(0);
    return 1; /* PC speaker is always available on x86. */
}

static void pcspk_backend_service(void)
{
    unsigned int n = audio_ring_read(g_pcspk_buf, PCSPK_BATCH);
    if (n == 0) {
        pc_speaker_tone(0);
        return;
    }

    /* Map average absolute sample value to a tone.  This is a very coarse
     * approximation, but it lets the user hear that audio is active. */
    int32_t sum = 0;
    for (unsigned int i = 0; i < n; i++) {
        sum += g_pcspk_buf[i * 2 + 0];
    }
    int avg = (int)(sum / (int32_t)n);
    int energy = avg < 0 ? -avg : avg;

    if (energy < 64) {
        pc_speaker_tone(0);
    } else {
        uint32_t freq = 220u + (uint32_t)(energy / 32);
        pc_speaker_tone(freq);
    }
}

static void pcspk_backend_shutdown(void)
{
    pc_speaker_tone(0);
}

AudioBackend audio_backend_pcspk = {
    .init     = pcspk_backend_init,
    .service  = pcspk_backend_service,
    .shutdown = pcspk_backend_shutdown,
    .name     = "PC-speaker",
};
