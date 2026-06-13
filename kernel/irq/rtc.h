/* rtc.h — UAOS CMOS Real-Time Clock driver */

#ifndef UAOS_RTC_H
#define UAOS_RTC_H

#include <stdint.h>

/* Time-only (HH:MM:SS) — used for the desktop clock tick */
typedef struct {
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
} RtcTime;

/* Full date + time */
typedef struct {
    uint16_t year;   /* e.g. 2026 */
    uint8_t  month;  /* 1–12      */
    uint8_t  day;    /* 1–31      */
    uint8_t  hour;   /* 0–23      */
    uint8_t  min;    /* 0–59      */
    uint8_t  sec;    /* 0–59      */
} RtcDateTime;

void        RTC_Init(void);
void        RTC_IRQHandler(uint64_t vec, uint64_t err);
RtcTime     RTC_ReadTime(void);             /* HH:MM:SS from cached snapshot   */
RtcDateTime RTC_ReadDateTime(void);         /* full date+time from CMOS        */

/*
 * Set the CMOS RTC to the given date/time (UTC).
 * Disables interrupts briefly; safe to call from task context.
 */
void RTC_SetDateTime(const RtcDateTime *dt);

#endif
