/*
 * timezone.c — Static IANA timezone table
 *
 * DST rule format: "last Sunday of <month> at <hour> local standard time"
 * expressed as month / week(5=last) / dow(0=Sun) / hour.
 *
 * Only a practical subset of zones is included; new zones can be added
 * to k_zones[] as needed.
 *
 * Southern-hemisphere DST: start is in October/November, end in March/April.
 * Northern-hemisphere DST: start is in March/April, end in October/November.
 */
#include "timezone.h"
#include "../net/ntp.h"   /* ntp_unix_to_datetime */

/* -------------------------------------------------------------------------
 * Helper: case-insensitive strcmp (no libc)
 * ------------------------------------------------------------------------- */
static int tz_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a; if (ca >= 'A' && ca <= 'Z') ca += 32;
        char cb = *b; if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* -------------------------------------------------------------------------
 * Static zone table
 *
 * Fields: name, abbr, dst_abbr,
 *         utc_offset_min, dst_offset_min,
 *         dst_start: month, week, dow, hour
 *         dst_end:   month, week, dow, hour
 *
 * week=5 means "last occurrence of that day-of-week in that month"
 * dow: 0=Sun, 1=Mon, … 6=Sat
 * ------------------------------------------------------------------------- */
static const TzInfo k_zones[] = {

    /* ── UTC ─────────────────────────────────────────────────────────────── */
    { "UTC",                  "UTC",  NULL,      0,   0,  0,0,0,0, 0,0,0,0 },
    { "Etc/UTC",              "UTC",  NULL,      0,   0,  0,0,0,0, 0,0,0,0 },
    { "GMT",                  "GMT",  NULL,      0,   0,  0,0,0,0, 0,0,0,0 },

    /* ── Australia ────────────────────────────────────────────────────────── */
    /* AEST = UTC+10, AEDT = UTC+11. DST: last Sun Oct 02:00 → last Sun Apr 03:00 */
    { "Australia/Sydney",     "AEST", "AEDT",  600,  60, 10,5,0,2, 4,1,0,3 },
    { "Australia/Melbourne",  "AEST", "AEDT",  600,  60, 10,5,0,2, 4,1,0,3 },
    { "Australia/Canberra",   "AEST", "AEDT",  600,  60, 10,5,0,2, 4,1,0,3 },
    { "Australia/Brisbane",   "AEST", NULL,    600,   0,  0,0,0,0, 0,0,0,0 },
    { "Australia/Hobart",     "AEST", "AEDT",  600,  60, 10,1,0,2, 4,1,0,3 },
    /* ACST = UTC+9:30, ACDT = UTC+10:30 */
    { "Australia/Adelaide",   "ACST", "ACDT",  570,  30, 10,5,0,2, 4,1,0,3 },
    { "Australia/Darwin",     "ACST", NULL,    570,   0,  0,0,0,0, 0,0,0,0 },
    /* AWST = UTC+8, no DST */
    { "Australia/Perth",      "AWST", NULL,    480,   0,  0,0,0,0, 0,0,0,0 },

    /* ── New Zealand ──────────────────────────────────────────────────────── */
    /* NZST = UTC+12, NZDT = UTC+13. DST: last Sun Sep 02:00 → first Sun Apr 03:00 */
    { "Pacific/Auckland",     "NZST", "NZDT",  720,  60,  9,5,0,2, 4,1,0,3 },

    /* ── United Kingdom ───────────────────────────────────────────────────── */
    /* GMT/BST: last Sun Mar 01:00 UTC → last Sun Oct 02:00 BST */
    { "Europe/London",        "GMT",  "BST",     0,  60,  3,5,0,1, 10,5,0,2 },

    /* ── Europe ───────────────────────────────────────────────────────────── */
    /* CET = UTC+1, CEST = UTC+2. DST: last Sun Mar 02:00 → last Sun Oct 03:00 */
    { "Europe/Paris",         "CET",  "CEST",   60,  60,  3,5,0,2, 10,5,0,3 },
    { "Europe/Berlin",        "CET",  "CEST",   60,  60,  3,5,0,2, 10,5,0,3 },
    { "Europe/Amsterdam",     "CET",  "CEST",   60,  60,  3,5,0,2, 10,5,0,3 },
    { "Europe/Rome",          "CET",  "CEST",   60,  60,  3,5,0,2, 10,5,0,3 },
    { "Europe/Madrid",        "CET",  "CEST",   60,  60,  3,5,0,2, 10,5,0,3 },
    /* EET = UTC+2, EEST = UTC+3 */
    { "Europe/Athens",        "EET",  "EEST",  120,  60,  3,5,0,3, 10,5,0,4 },
    { "Europe/Helsinki",      "EET",  "EEST",  120,  60,  3,5,0,3, 10,5,0,4 },
    /* MSK = UTC+3, no DST since 2014 */
    { "Europe/Moscow",        "MSK",  NULL,    180,   0,  0,0,0,0, 0,0,0,0 },

    /* ── USA ──────────────────────────────────────────────────────────────── */
    /* EST = UTC-5, EDT = UTC-4. DST: 2nd Sun Mar 02:00 → 1st Sun Nov 02:00 */
    { "America/New_York",     "EST",  "EDT",  -300,  60,  3,2,0,2, 11,1,0,2 },
    { "America/Toronto",      "EST",  "EDT",  -300,  60,  3,2,0,2, 11,1,0,2 },
    /* CST = UTC-6, CDT = UTC-5 */
    { "America/Chicago",      "CST",  "CDT",  -360,  60,  3,2,0,2, 11,1,0,2 },
    /* MST = UTC-7, MDT = UTC-6 */
    { "America/Denver",       "MST",  "MDT",  -420,  60,  3,2,0,2, 11,1,0,2 },
    { "America/Phoenix",      "MST",  NULL,   -420,   0,  0,0,0,0, 0,0,0,0 },
    /* PST = UTC-8, PDT = UTC-7 */
    { "America/Los_Angeles",  "PST",  "PDT",  -480,  60,  3,2,0,2, 11,1,0,2 },
    { "America/Vancouver",    "PST",  "PDT",  -480,  60,  3,2,0,2, 11,1,0,2 },
    /* AKST = UTC-9, AKDT = UTC-8 */
    { "America/Anchorage",    "AKST", "AKDT", -540,  60,  3,2,0,2, 11,1,0,2 },
    /* HST = UTC-10, no DST */
    { "Pacific/Honolulu",     "HST",  NULL,   -600,   0,  0,0,0,0, 0,0,0,0 },

    /* ── Asia ─────────────────────────────────────────────────────────────── */
    { "Asia/Tokyo",           "JST",  NULL,    540,   0,  0,0,0,0, 0,0,0,0 },
    { "Asia/Shanghai",        "CST",  NULL,    480,   0,  0,0,0,0, 0,0,0,0 },
    { "Asia/Hong_Kong",       "HKT",  NULL,    480,   0,  0,0,0,0, 0,0,0,0 },
    { "Asia/Singapore",       "SGT",  NULL,    480,   0,  0,0,0,0, 0,0,0,0 },
    { "Asia/Kolkata",         "IST",  NULL,    330,   0,  0,0,0,0, 0,0,0,0 },
    { "Asia/Dubai",           "GST",  NULL,    240,   0,  0,0,0,0, 0,0,0,0 },
    { "Asia/Riyadh",          "AST",  NULL,    180,   0,  0,0,0,0, 0,0,0,0 },

    /* ── Sentinel ─────────────────────────────────────────────────────────── */
    { NULL, NULL, NULL, 0, 0, 0,0,0,0, 0,0,0,0 }
};

/* -------------------------------------------------------------------------
 * Global current timezone
 * ------------------------------------------------------------------------- */
static const TzInfo *g_current_tz = &k_zones[0];   /* defaults to UTC */

void          tz_set_current(const TzInfo *tz) { if (tz) g_current_tz = tz; }
const TzInfo *tz_get_current(void)             { return g_current_tz; }

/* -------------------------------------------------------------------------
 * Public lookup
 * ------------------------------------------------------------------------- */
const TzInfo *tz_find(const char *name)
{
    if (!name || !*name) return NULL;
    for (int i = 0; k_zones[i].name; i++)
        if (tz_ieq(k_zones[i].name, name))
            return &k_zones[i];
    return NULL;
}

/* -------------------------------------------------------------------------
 * DST calculation
 *
 * Given a UTC Unix timestamp, determine the UTC epoch second at which a
 * Mn.w.d/h transition occurs for the given (year, month, week, dow, hour).
 *
 * The "wall clock hour" (h) is expressed in *local standard time*, so we
 * convert it to UTC by subtracting the standard offset.
 * ------------------------------------------------------------------------- */

/* Days in a month (non-leap) */
static const uint8_t k_days_in_month[13] = {
    0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static int is_leap(uint16_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

/* Return the Unix timestamp (UTC) for the transition described by
 * month/week/dow/hour in local standard time for the given year.
 * standard_offset_min: the zone's standard UTC offset (no DST). */
static uint32_t transition_epoch(uint16_t year, uint8_t month,
                                  uint8_t week, uint8_t dow,
                                  uint8_t hour_local,
                                  int16_t standard_offset_min)
{
    /* Find the first day-of-month that matches dow */
    /* Jan 1 1970 was a Thursday (dow=4).  Compute the day-of-week of
     * the 1st of the given month by counting days from epoch. */

    /* Days from 1970-01-01 to the 1st of month in year */
    /* Use a simple loop — called at most a handful of times per boot */
    uint32_t days = 0;
    for (uint16_t y = 1970; y < year; y++)
        days += is_leap(y) ? 366 : 365;
    for (uint8_t m = 1; m < month; m++) {
        uint8_t d = k_days_in_month[m];
        if (m == 2 && is_leap(year)) d++;
        days += d;
    }

    /* day-of-week of the 1st of this month (0=Thu offset from 1970-01-01 Thu) */
    int first_dow = (int)((days + 4) % 7);   /* +4 because Jan 1 1970 = Thu = 4 */

    /* Find how many days until the first occurrence of 'dow' */
    int offset = ((int)dow - first_dow + 7) % 7;   /* 0–6 */

    uint8_t dim = k_days_in_month[month];
    if (month == 2 && is_leap(year)) dim++;

    uint32_t day1 = days + (uint32_t)offset;   /* 1st occurrence (0-based from epoch) */

    uint32_t target_day;
    if (week == 5) {
        /* Last occurrence: keep adding 7 until we'd go past the month */
        target_day = day1;
        while (target_day + 7 < days + dim)
            target_day += 7;
    } else {
        target_day = day1 + (uint32_t)(week - 1) * 7;
        /* Safety clamp */
        if (target_day >= days + dim)
            target_day = day1;
    }

    /* Convert wall hour (local standard) to UTC second */
    uint32_t wall_utc = target_day * 86400U
                      + (uint32_t)hour_local * 3600U
                      - (uint32_t)((int32_t)standard_offset_min * 60);
    return wall_utc;
}

int tz_is_dst(const TzInfo *tz, uint32_t unix_utc)
{
    if (!tz || tz->dst_offset_min == 0) return 0;

    /* Decompose UTC timestamp to get the year */
    uint16_t year; uint8_t month, day, hour, min, sec;
    ntp_unix_to_datetime(unix_utc, &year, &month, &day, &hour, &min, &sec);

    uint32_t t_start = transition_epoch(year,
                                        tz->dst_start_month,
                                        tz->dst_start_week,
                                        tz->dst_start_dow,
                                        tz->dst_start_hour,
                                        tz->utc_offset_min);

    uint32_t t_end   = transition_epoch(year,
                                        tz->dst_end_month,
                                        tz->dst_end_week,
                                        tz->dst_end_dow,
                                        tz->dst_end_hour,
                                        tz->utc_offset_min);

    /* Northern hemisphere: DST is from start → end within the same year */
    if (t_start < t_end)
        return unix_utc >= t_start && unix_utc < t_end;

    /* Southern hemisphere (e.g. Australia): DST spans year-end.
     * Active when: after t_start OR before t_end */
    return unix_utc >= t_start || unix_utc < t_end;
}

int32_t tz_offset_min(const TzInfo *tz, uint32_t unix_utc)
{
    if (!tz) return 0;
    int32_t off = (int32_t)tz->utc_offset_min;
    if (tz_is_dst(tz, unix_utc))
        off += (int32_t)tz->dst_offset_min;
    return off;
}

const char *tz_abbr(const TzInfo *tz, uint32_t unix_utc)
{
    if (!tz) return "UTC";
    if (tz_is_dst(tz, unix_utc) && tz->dst_abbr)
        return tz->dst_abbr;
    return tz->abbr ? tz->abbr : "UTC";
}
