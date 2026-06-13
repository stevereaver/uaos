/*
 * timezone.h — Static IANA timezone table for UAOS
 *
 * Provides UTC offset lookup by IANA zone name (e.g. "Australia/Sydney").
 * DST is handled via a small rule set: zones that observe DST list the
 * month/week/day the clocks change and by how many minutes.
 *
 * The CMOS RTC always stores UTC.  Local time is computed on the fly
 * by adding the UTC offset + (DST if in effect) to the UTC timestamp.
 */
#ifndef UAOS_TIMEZONE_H
#define UAOS_TIMEZONE_H

#include <stdint.h>

/*
 * tz_info — everything needed to convert UTC → local time.
 *
 * utc_offset_min  : standard UTC offset in minutes (e.g. +600 for AEST)
 * dst_offset_min  : extra minutes when DST is active (0 = no DST)
 *
 * DST transition rules (POSIX Mn.w.d style):
 *   dst_start / dst_end:
 *     month  : 1–12
 *     week   : 1–5  (5 = last occurrence)
 *     dow    : 0=Sun … 6=Sat
 *     hour   : wall-clock hour of transition (in LOCAL standard time)
 *
 * If dst_offset_min == 0 the dst_start/end fields are ignored.
 *
 * abbr / dst_abbr : short timezone abbreviation strings (e.g. "AEST", "AEDT")
 */
typedef struct {
    const char *name;            /* IANA zone name, e.g. "Australia/Sydney"  */
    const char *abbr;            /* standard abbreviation, e.g. "AEST"       */
    const char *dst_abbr;        /* DST abbreviation, e.g. "AEDT" (or NULL)  */
    int16_t     utc_offset_min;  /* standard offset in minutes               */
    int16_t     dst_offset_min;  /* DST extra minutes (0 = no DST)           */
    /* DST start: clocks forward */
    uint8_t  dst_start_month;
    uint8_t  dst_start_week;     /* occurrence: 1=first, 5=last              */
    uint8_t  dst_start_dow;      /* 0=Sun … 6=Sat                            */
    uint8_t  dst_start_hour;     /* wall hour (local standard time)          */
    /* DST end: clocks back */
    uint8_t  dst_end_month;
    uint8_t  dst_end_week;
    uint8_t  dst_end_dow;
    uint8_t  dst_end_hour;
} TzInfo;

/*
 * Look up a timezone by IANA name.  Comparison is case-insensitive.
 * Returns a pointer into the static table on success, NULL if not found.
 */
const TzInfo *tz_find(const char *name);

/*
 * Given a UTC Unix timestamp, return 1 if DST is currently in effect for
 * the given zone, 0 otherwise.
 */
int tz_is_dst(const TzInfo *tz, uint32_t unix_utc);

/*
 * Compute local Unix timestamp = unix_utc + UTC offset + DST offset.
 * Returns the full local offset in minutes (positive = east of UTC).
 */
int32_t tz_offset_min(const TzInfo *tz, uint32_t unix_utc);

/* Convenience: return the appropriate abbreviation for the current moment */
const char *tz_abbr(const TzInfo *tz, uint32_t unix_utc);

/* Global configured timezone (set by ntpd from timezone.conf) */
void           tz_set_current(const TzInfo *tz);
const TzInfo  *tz_get_current(void);

#endif /* UAOS_TIMEZONE_H */
