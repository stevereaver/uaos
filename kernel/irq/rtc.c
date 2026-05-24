/* rtc.c — UAOS CMOS Real-Time Clock driver
 *
 * Uses the MC146818 CMOS RTC:
 *   - Port 0x70: index register (NMI disable bit = bit 7)
 *   - Port 0x71: data register
 *
 * Enables the RTC Update-Ended interrupt (UIE) on IRQ8 so the clock
 * ticks exactly once per second.  The IRQ handler snapshots the time into
 * a cached global; Desktop_UpdateClock() reads that cache — no CMOS spin
 * inside interrupt context.
 */

#include "rtc.h"
#include "../display/desktop.h"
#include <stdint.h>

/* -------------------------------------------------------------------------
 * I/O helpers
 * ------------------------------------------------------------------------- */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* CMOS register read (NMI disabled via bit 7) */
static uint8_t cmos_read(uint8_t reg)
{
    outb(0x70, (uint8_t)(0x80 | reg));
    return inb(0x71);
}

/* -------------------------------------------------------------------------
 * BCD → binary
 * ------------------------------------------------------------------------- */

static inline uint8_t bcd2bin(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F));
}

/* -------------------------------------------------------------------------
 * Cached time — updated by IRQ handler, read by Desktop_UpdateClock
 * ------------------------------------------------------------------------- */

static volatile uint8_t g_hour;
static volatile uint8_t g_min;
static volatile uint8_t g_sec;

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

RtcTime RTC_ReadTime(void)
{
    RtcTime t = { (uint8_t)g_hour, (uint8_t)g_min, (uint8_t)g_sec };
    return t;
}

/* Read time directly from CMOS (call only outside IRQ context) */
static void rtc_snapshot(void)
{
    /* Spin-wait is safe here — called at init, not from IRQ */
    while (cmos_read(0x0A) & 0x80)
        ;

    uint8_t s  = cmos_read(0x00);
    uint8_t m  = cmos_read(0x02);
    uint8_t h  = cmos_read(0x04);
    uint8_t sb = cmos_read(0x0B);

    if (!(sb & 0x04)) {
        s = bcd2bin(s);
        m = bcd2bin(m);
        if (!(sb & 0x02)) {
            uint8_t pm = h & 0x80;
            h = bcd2bin(h & 0x7F);
            if (pm && h != 12) h += 12;
            if (!pm && h == 12) h = 0;
        } else {
            h = bcd2bin(h);
        }
    }

    g_hour = h;
    g_min  = m;
    g_sec  = s;
}

void RTC_IRQHandler(uint64_t vec, uint64_t err)
{
    (void)vec; (void)err;

    /* Must read register C to clear the interrupt flag — without this
     * the RTC will not generate any further IRQ8s. */
    outb(0x70, 0x0C);
    inb(0x71);

    /* Update cached time from CMOS (UIP is clear at UIE interrupt time) */
    uint8_t s  = cmos_read(0x00);
    uint8_t m  = cmos_read(0x02);
    uint8_t h  = cmos_read(0x04);
    uint8_t sb = cmos_read(0x0B);

    if (!(sb & 0x04)) {
        s = bcd2bin(s);
        m = bcd2bin(m);
        if (!(sb & 0x02)) {
            uint8_t pm = h & 0x80;
            h = bcd2bin(h & 0x7F);
            if (pm && h != 12) h += 12;
            if (!pm && h == 12) h = 0;
        } else {
            h = bcd2bin(h);
        }
    }

    g_hour = h;
    g_min  = m;
    g_sec  = s;

    /* Redraw clock in menu bar */
    Desktop_UpdateClock();
}

void RTC_Init(void)
{
    /* Snapshot current time before enabling IRQ */
    rtc_snapshot();

    /* Enable UIE (Update-Ended Interrupt, register B bit 4) */
    outb(0x70, 0x8B);
    uint8_t prev = inb(0x71);
    outb(0x70, 0x8B);
    outb(0x71, (uint8_t)(prev | 0x10));

    /* Clear any pending interrupt in register C */
    outb(0x70, 0x0C);
    inb(0x71);
}
