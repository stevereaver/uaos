/* rtc.c — UAOS CMOS Real-Time Clock driver
 *
 * Uses the MC146818 CMOS RTC:
 *   - Port 0x70: index register (NMI disable bit = bit 7)
 *   - Port 0x71: data register
 *
 * CMOS register map (relevant subset):
 *   0x00 seconds   0x02 minutes   0x04 hours
 *   0x07 day       0x08 month     0x09 year (0–99)
 *   0x32 century   0x0A status A  0x0B status B
 *
 * Enables the RTC Update-Ended interrupt (UIE) on IRQ8 so the clock
 * ticks exactly once per second.  The IRQ handler snapshots the time into
 * a cached global; Desktop_UpdateClock() reads that cache — no CMOS spin
 * inside interrupt context.
 */

#include "rtc.h"
#include "../display/desktop.h"
#include "../net/ntp.h"
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

/* CMOS register write (NMI disabled via bit 7) */
static void cmos_write(uint8_t reg, uint8_t val)
{
    outb(0x70, (uint8_t)(0x80 | reg));
    outb(0x71, val);
}

/* -------------------------------------------------------------------------
 * BCD ↔ binary helpers
 * ------------------------------------------------------------------------- */

static inline uint8_t bcd2bin(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F));
}

static inline uint8_t bin2bcd(uint8_t bin)
{
    return (uint8_t)(((bin / 10) << 4) | (bin % 10));
}

/* -------------------------------------------------------------------------
 * Cached snapshot — updated by IRQ handler once per second
 * ------------------------------------------------------------------------- */

static volatile uint8_t  g_sec;
static volatile uint8_t  g_min;
static volatile uint8_t  g_hour;
static volatile uint8_t  g_day;
static volatile uint8_t  g_month;
static volatile uint16_t g_year;

/* -------------------------------------------------------------------------
 * Internal: decode raw CMOS values respecting BCD / binary and 12/24h mode
 * ------------------------------------------------------------------------- */
static void rtc_decode(uint8_t raw_s, uint8_t raw_m, uint8_t raw_h,
                       uint8_t raw_day, uint8_t raw_mon, uint8_t raw_yr,
                       uint8_t raw_cent, uint8_t status_b)
{
    uint8_t is_binary = status_b & 0x04;   /* bit2: 1=binary, 0=BCD */
    uint8_t is_24h    = status_b & 0x02;   /* bit1: 1=24h,    0=12h  */

    uint8_t s   = is_binary ? raw_s   : bcd2bin(raw_s);
    uint8_t m   = is_binary ? raw_m   : bcd2bin(raw_m);
    uint8_t day = is_binary ? raw_day : bcd2bin(raw_day);
    uint8_t mon = is_binary ? raw_mon : bcd2bin(raw_mon);

    uint8_t h;
    if (is_binary) {
        h = raw_h & 0x7F;
        if (!is_24h) {
            uint8_t pm = raw_h & 0x80;
            if (pm && h != 12) h += 12;
            if (!pm && h == 12) h = 0;
        }
    } else {
        uint8_t pm = (!is_24h) ? (raw_h & 0x80) : 0;
        h = bcd2bin(raw_h & 0x7F);
        if (pm && h != 12) h += 12;
        if (!pm && h == 12 && !is_24h) h = 0;
    }

    uint8_t yr   = is_binary ? raw_yr   : bcd2bin(raw_yr);
    uint8_t cent = is_binary ? raw_cent : bcd2bin(raw_cent);
    uint16_t year = cent ? (uint16_t)(cent * 100 + yr)
                         : (uint16_t)(2000 + yr);   /* assume 21st century if no century reg */

    g_sec   = s;
    g_min   = m;
    g_hour  = h;
    g_day   = day;
    g_month = mon;
    g_year  = year;
}

/* -------------------------------------------------------------------------
 * Public read API
 * ------------------------------------------------------------------------- */

RtcTime RTC_ReadTime(void)
{
    RtcTime t = { (uint8_t)g_hour, (uint8_t)g_min, (uint8_t)g_sec };
    return t;
}

RtcDateTime RTC_ReadDateTime(void)
{
    RtcDateTime dt;
    dt.year  = g_year;
    dt.month = (uint8_t)g_month;
    dt.day   = (uint8_t)g_day;
    dt.hour  = (uint8_t)g_hour;
    dt.min   = (uint8_t)g_min;
    dt.sec   = (uint8_t)g_sec;
    return dt;
}

/* -------------------------------------------------------------------------
 * Public write API — set CMOS to a new date/time
 * The CMOS is always written in BCD 24h format for simplicity.
 * We disable NMI (via the 0x80 bit on port 0x70), set the SET bit in
 * register B to freeze the clock, write all registers, then clear SET.
 * ------------------------------------------------------------------------- */
void RTC_SetDateTime(const RtcDateTime *dt)
{
    /* Wait for any update in progress to finish */
    while (cmos_read(0x0A) & 0x80)
        ;

    /* Set the SET bit (bit 7 of register B) to halt clock updates */
    uint8_t regB = cmos_read(0x0B);
    cmos_write(0x0B, (uint8_t)(regB | 0x80));

    /* Write time/date in BCD, 24h format */
    cmos_write(0x00, bin2bcd(dt->sec));
    cmos_write(0x02, bin2bcd(dt->min));
    cmos_write(0x04, bin2bcd(dt->hour));
    cmos_write(0x07, bin2bcd(dt->day));
    cmos_write(0x08, bin2bcd(dt->month));
    cmos_write(0x09, bin2bcd((uint8_t)(dt->year % 100)));
    cmos_write(0x32, bin2bcd((uint8_t)(dt->year / 100)));

    /* Force binary=0 (BCD), 24h=1 so our reads always agree */
    cmos_write(0x0B, (uint8_t)((regB & ~0x84) | 0x02));

    /* Update the in-memory cache immediately */
    g_sec   = dt->sec;
    g_min   = dt->min;
    g_hour  = dt->hour;
    g_day   = dt->day;
    g_month = dt->month;
    g_year  = dt->year;
}

/* -------------------------------------------------------------------------
 * Snapshot from CMOS (called at init and optionally externally)
 * ------------------------------------------------------------------------- */
static void rtc_snapshot(void)
{
    while (cmos_read(0x0A) & 0x80)
        ;

    uint8_t raw_s    = cmos_read(0x00);
    uint8_t raw_m    = cmos_read(0x02);
    uint8_t raw_h    = cmos_read(0x04);
    uint8_t raw_day  = cmos_read(0x07);
    uint8_t raw_mon  = cmos_read(0x08);
    uint8_t raw_yr   = cmos_read(0x09);
    uint8_t raw_cent = cmos_read(0x32);
    uint8_t status_b = cmos_read(0x0B);

    rtc_decode(raw_s, raw_m, raw_h, raw_day, raw_mon, raw_yr,
               raw_cent, status_b);
}

/* -------------------------------------------------------------------------
 * IRQ8 handler — fires once per second (UIE)
 * ------------------------------------------------------------------------- */
void RTC_IRQHandler(uint64_t vec, uint64_t err)
{
    (void)vec; (void)err;

    /* Must read register C to clear the interrupt flag */
    outb(0x70, 0x0C);
    inb(0x71);

    /* Read all time/date registers from CMOS */
    uint8_t raw_s    = cmos_read(0x00);
    uint8_t raw_m    = cmos_read(0x02);
    uint8_t raw_h    = cmos_read(0x04);
    uint8_t raw_day  = cmos_read(0x07);
    uint8_t raw_mon  = cmos_read(0x08);
    uint8_t raw_yr   = cmos_read(0x09);
    uint8_t raw_cent = cmos_read(0x32);
    uint8_t status_b = cmos_read(0x0B);

    rtc_decode(raw_s, raw_m, raw_h, raw_day, raw_mon, raw_yr,
               raw_cent, status_b);

    /* Advance the live UTC epoch counter */
    ntp_tick_epoch();

    /* Redraw clock in menu bar */
    Desktop_UpdateClock();
}

/* -------------------------------------------------------------------------
 * Init
 * ------------------------------------------------------------------------- */
void RTC_Init(void)
{
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
