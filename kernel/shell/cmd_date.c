/*
 * cmd_date.c — C:date — display current local date and time
 *
 * When ntpd has run, uses the live UTC epoch counter + current timezone
 * to display local time with the correct abbreviation.
 *
 * When ntpd has not run, falls back to the CMOS RTC (shows UTC).
 *
 * Output format (AmigaDOS-style):
 *   Saturday 14-Jun-2026 23:12:57 AEST (Australia/Sydney)
 */
#include "cmd_internal.h"
#include "../irq/rtc.h"
#include "../net/ntp.h"
#include "../net/timezone.h"

static const char *const k_months[] = {
    "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char *const k_days[] = {
    "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
};

static int day_of_week(uint16_t y, uint8_t m, uint8_t d)
{
    static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y--;
    return (int)((y + y/4 - y/100 + y/400 + t[m-1] + d) % 7);
}

static void dig2(uint8_t v, char *out)
{
    out[0] = (char)('0' + v / 10);
    out[1] = (char)('0' + v % 10);
}

void Cmd_Date(NativeCmdCtx *ctx, const char *args)
{
    (void)args;

    uint16_t year; uint8_t month, day, hour, min, sec;
    const char *abbr;
    const char *tz_name = NULL;

    uint32_t epoch = ntp_get_epoch();
    if (epoch) {
        /* NTP-synced: apply timezone */
        const TzInfo *tz   = tz_get_current();
        int32_t  off_min   = tz_offset_min(tz, epoch);
        uint32_t local_ts  = (uint32_t)((int64_t)epoch + (int64_t)off_min * 60);
        ntp_unix_to_datetime(local_ts, &year, &month, &day, &hour, &min, &sec);
        abbr    = tz_abbr(tz, epoch);
        tz_name = (tz && tz->name) ? tz->name : NULL;
    } else {
        /* Fallback: CMOS directly (UTC) */
        RtcDateTime dt = RTC_ReadDateTime();
        year  = dt.year; month = dt.month; day = dt.day;
        hour  = dt.hour; min   = dt.min;   sec = dt.sec;
        abbr  = "UTC";
        tz_name = NULL;

        if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31) {
            PRINT("date: clock not set (run ntpd to synchronise)");
            return;
        }
    }

    int dow = day_of_week(year, month, day);
    if (dow < 0 || dow > 6) dow = 0;

    char line[128];
    char *p = line;

    /* Day name */
    const char *dn = k_days[dow];
    while (*dn) *p++ = *dn++;
    *p++ = ' ';

    /* DD-Mon-YYYY */
    dig2(day, p); p += 2; *p++ = '-';
    const char *mn = k_months[month < 13 ? month : 0];
    while (*mn) *p++ = *mn++;
    *p++ = '-';
    uint16_t yr = year;
    *p++ = (char)('0' + (yr/1000)%10);
    *p++ = (char)('0' + (yr/100) %10);
    *p++ = (char)('0' + (yr/10)  %10);
    *p++ = (char)('0' + (yr)     %10);
    *p++ = ' ';

    /* HH:MM:SS */
    dig2(hour, p); p += 2; *p++ = ':';
    dig2(min,  p); p += 2; *p++ = ':';
    dig2(sec,  p); p += 2;

    /* Timezone abbreviation */
    *p++ = ' ';
    while (*abbr) *p++ = *abbr++;

    /* Full timezone name in parens if available */
    if (tz_name) {
        *p++ = ' '; *p++ = '(';
        while (*tz_name) *p++ = *tz_name++;
        *p++ = ')';
    }
    *p = '\0';

    PRINT(line);
    (void)ctx;
}
