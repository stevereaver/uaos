/*
 * cmd_date.c — C:date — display current date and time from the RTC
 *
 * Reads the live date/time from the CMOS real-time clock.
 * After ntpd runs, this will reflect the NTP-synchronised UTC time.
 *
 * Output format (mimics AmigaDOS date):
 *   Saturday 14-Jun-2026 13:45:22 UTC
 */
#include "cmd_internal.h"
#include "../irq/rtc.h"

static const char *const k_months[] = {
    "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char *const k_days[] = {
    "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
};

/* Determine day-of-week using Tomohiko Sakamoto's algorithm (Gregorian) */
static int day_of_week(uint16_t y, uint8_t m, uint8_t d)
{
    static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y--;
    return (int)((y + y/4 - y/100 + y/400 + t[m-1] + d) % 7);
}

/* Zero-padded two-digit decimal into buf[2]; does NOT NUL-terminate */
static void dig2(uint8_t v, char *out)
{
    out[0] = (char)('0' + v / 10);
    out[1] = (char)('0' + v % 10);
}

void Cmd_Date(NativeCmdCtx *ctx, const char *args)
{
    (void)args;

    RtcDateTime dt = RTC_ReadDateTime();

    /* Sanity check: year must be plausible */
    if (dt.year < 2000 || dt.year > 2099 ||
        dt.month < 1   || dt.month > 12  ||
        dt.day   < 1   || dt.day   > 31) {
        PRINT("date: RTC not set (run ntpd to synchronise)");
        return;
    }

    /* Day-of-week name */
    int dow = day_of_week(dt.year, dt.month, dt.day);
    if (dow < 0 || dow > 6) dow = 0;

    /* Build: "Saturday 14-Jun-2026 13:45:22 UTC" */
    char line[64];
    char *p = line;

    /* Day name */
    const char *dn = k_days[dow];
    while (*dn) *p++ = *dn++;
    *p++ = ' ';

    /* DD-Mon-YYYY */
    dig2(dt.day, p); p += 2; *p++ = '-';
    const char *mn = k_months[dt.month < 13 ? dt.month : 0];
    while (*mn) *p++ = *mn++;
    *p++ = '-';
    uint16_t yr = dt.year;
    *p++ = (char)('0' + (yr / 1000) % 10);
    *p++ = (char)('0' + (yr / 100)  % 10);
    *p++ = (char)('0' + (yr / 10)   % 10);
    *p++ = (char)('0' + (yr)        % 10);
    *p++ = ' ';

    /* HH:MM:SS */
    dig2(dt.hour, p); p += 2; *p++ = ':';
    dig2(dt.min,  p); p += 2; *p++ = ':';
    dig2(dt.sec,  p); p += 2;

    /* Timezone */
    const char *tz = " UTC";
    while (*tz) *p++ = *tz++;
    *p = '\0';

    PRINT(line);
}
