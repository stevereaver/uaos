/* rtc.h — UAOS CMOS Real-Time Clock driver */

#ifndef UAOS_RTC_H
#define UAOS_RTC_H

#include <stdint.h>

typedef struct {
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
} RtcTime;

void    RTC_Init(void);                     /* enable periodic IRQ8 at 1 Hz  */
void    RTC_IRQHandler(uint64_t vec, uint64_t err);
RtcTime RTC_ReadTime(void);                 /* read current HH:MM:SS from CMOS */

#endif
