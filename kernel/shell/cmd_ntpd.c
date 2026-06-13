/*
 * cmd_ntpd.c — UAOS ntpd command
 *
 * Performs a one-shot NTP time synchronisation and sets the system clock.
 *
 * Usage:
 *   ntpd                  — sync from pool.ntp.org (default)
 *   ntpd <server>         — sync from a specific hostname or IP
 *
 * Typical startup-sequence usage:
 *   C:ntpd >NIL:
 *
 * On success:  sets the CMOS RTC and prints the new date/time.
 * On failure:  prints an error and leaves the clock unchanged.
 */
#include "cmd_internal.h"
#include "../net/stack.h"
#include "../net/dns.h"
#include "../net/ntp.h"
#include "../irq/rtc.h"

#define NTP_DEFAULT_SERVER  "pool.ntp.org"

/* Shared poll adapter — yields the UI while waiting for DNS / NTP */
static void ntpd_poll(void *arg, uint32_t ms)
{
    CMD_YIELD((NativeCmdCtx *)arg, ms);
}

/* -------------------------------------------------------------------------
 * uint32 → decimal string (no libc)
 * ------------------------------------------------------------------------- */
static void u32dec(uint32_t v, char *buf, int max)
{
    char tmp[12]; int i = 0, j = 0;
    if (!v) { buf[j++] = '0'; buf[j] = '\0'; return; }
    while (v && i < 11) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i-- && j < max - 1) buf[j++] = tmp[i];
    buf[j] = '\0';
}

/* Zero-padded two-digit decimal */
static void u8_2dig(uint8_t v, char *out)
{
    out[0] = (char)('0' + v / 10);
    out[1] = (char)('0' + v % 10);
}

/* -------------------------------------------------------------------------
 * Command entry point
 * ------------------------------------------------------------------------- */
void Cmd_Ntpd(NativeCmdCtx *ctx, const char *args)
{
    if (!net_stack_is_up()) {
        PRINT("ntpd: network stack not available");
        return;
    }

    /* Server name: arg or default */
    char server[256];
    if (args && *args) {
        int i = 0;
        while (*args && *args != ' ' && i < 255) server[i++] = *args++;
        server[i] = '\0';
    } else {
        int i = 0;
        const char *def = NTP_DEFAULT_SERVER;
        while (*def && i < 255) server[i++] = *def++;
        server[i] = '\0';
    }

    /* ---- Step 1: resolve server hostname ---- */
    char line[128];
    cmd_scopy(line, "ntpd: resolving ", sizeof(line));
    cmd_scat(line, server, sizeof(line));
    cmd_scat(line, "...", sizeof(line));
    PRINT(line);

    ipv4_t srv_ip = 0;
    if (!dns_resolve(server, &srv_ip, 5000, ntpd_poll, ctx)) {
        cmd_scopy(line, "ntpd: cannot resolve '", sizeof(line));
        cmd_scat(line, server, sizeof(line));
        cmd_scat(line, "'", sizeof(line));
        PRINT(line);
        return;
    }

    char ip_s[20];
    net_ip_to_str(srv_ip, ip_s);
    cmd_scopy(line, "ntpd: querying ", sizeof(line));
    cmd_scat(line, ip_s, sizeof(line));
    cmd_scat(line, "...", sizeof(line));
    PRINT(line);

    /* ---- Step 2: SNTP query ---- */
    uint32_t unix_ts = 0;
    if (!ntp_query(srv_ip, &unix_ts, 5000, ntpd_poll, ctx)) {
        PRINT("ntpd: no reply from NTP server");
        return;
    }

    /* ---- Step 3: decompose timestamp ---- */
    uint16_t year; uint8_t month, day, hour, min, sec;
    ntp_unix_to_datetime(unix_ts, &year, &month, &day, &hour, &min, &sec);

    /* ---- Step 4: write to CMOS RTC ---- */
    RtcDateTime dt;
    dt.year  = year;
    dt.month = month;
    dt.day   = day;
    dt.hour  = hour;
    dt.min   = min;
    dt.sec   = sec;
    RTC_SetDateTime(&dt);

    /* ---- Step 5: print confirmation ---- */
    /*  "ntpd: clock set to 2026-06-14 13:45:22 UTC"  */
    char yr_s[8];
    u32dec(year, yr_s, sizeof(yr_s));

    char mon_s[3], day_s[3], hr_s[3], mn_s[3], sc_s[3];
    u8_2dig(month, mon_s); mon_s[2] = '\0';
    u8_2dig(day,   day_s); day_s[2] = '\0';
    u8_2dig(hour,  hr_s);  hr_s[2]  = '\0';
    u8_2dig(min,   mn_s);  mn_s[2]  = '\0';
    u8_2dig(sec,   sc_s);  sc_s[2]  = '\0';

    cmd_scopy(line, "ntpd: clock set to ", sizeof(line));
    cmd_scat(line, yr_s,  sizeof(line)); cmd_scat(line, "-", sizeof(line));
    cmd_scat(line, mon_s, sizeof(line)); cmd_scat(line, "-", sizeof(line));
    cmd_scat(line, day_s, sizeof(line)); cmd_scat(line, " ", sizeof(line));
    cmd_scat(line, hr_s,  sizeof(line)); cmd_scat(line, ":", sizeof(line));
    cmd_scat(line, mn_s,  sizeof(line)); cmd_scat(line, ":", sizeof(line));
    cmd_scat(line, sc_s,  sizeof(line)); cmd_scat(line, " UTC", sizeof(line));
    PRINT(line);
}
