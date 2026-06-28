/*
 * pc_speaker.c — PC speaker audio backend
 *
 * Uses the legacy PC speaker (PIT channel 2 and gate port 0x61) to produce a
 * square wave.  This is a minimal backend suitable for QEMU and many PCs; it is
 * not high fidelity.
 */

#include "audio/audio.h"
#include <stdint.h>

/* x86 I/O port definitions */
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
        /* Turn speaker off by clearing the gate bit. */
        uint8_t ctl = inb(SPEAKER_CTL);
        outb(SPEAKER_CTL, (uint8_t)(ctl & ~0x03u));
        return;
    }

    /* PIT channel 2 runs at 1.193182 MHz. */
    uint32_t divisor = 1193182u / freq_hz;
    if (divisor > 0xFFFFu) divisor = 0xFFFFu;
    if (divisor < 1) divisor = 1;

    outb(PIT_CMD, 0xB6u); /* channel 2, square wave, low/high byte */
    outb(PIT_DATA2, (uint8_t)(divisor & 0xFFu));
    outb(PIT_DATA2, (uint8_t)((divisor >> 8) & 0xFFu));

    /* Connect channel 2 to the speaker. */
    uint8_t ctl = inb(SPEAKER_CTL);
    outb(SPEAKER_CTL, (uint8_t)(ctl | 0x03u));
}
