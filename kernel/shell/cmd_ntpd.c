/*
 * cmd_ntpd.c — UAOS ntpd command
 *
 * One-shot NTP time synchronisation.  Reads two config files from C:
 *
 *   C:ntp.conf      — NTP server hostname, one per line; first line used.
 *                     Lines beginning with '#' are comments.
 *                     Default: pool.ntp.org
 *
 *   C:timezone.conf — IANA timezone name on the first non-comment line.
 *                     Default: Australia/Sydney
 *
 * The CMOS RTC is always set to UTC.  The timezone is stored in the global
 * timezone state (tz_set_current) so that the clock window and date command
 * can display local time on the fly.
 *
 * Usage:
 *   ntpd                  — use C:ntp.conf / C:timezone.conf
 *   ntpd <server>         — override server (timezone.conf still read)
 */
#include "cmd_internal.h"
#include "../net/stack.h"
#include "../net/dns.h"
#include "../net/ntp.h"
#include "../net/timezone.h"
#include "../irq/rtc.h"

#define NTP_DEFAULT_SERVER  "pool.ntp.org"
#define TZ_DEFAULT          "Australia/Sydney"
#define NTP_CONF_PATH       "S:ntp.conf"
#define TZ_CONF_PATH        "S:timezone.conf"

/* Shared poll adapter */
static void ntpd_poll(void *arg, uint32_t ms) { CMD_YIELD((NativeCmdCtx *)arg, ms); }

/* -------------------------------------------------------------------------
 * Utility: uint32 and 2-digit decimal formatters (no libc)
 * ------------------------------------------------------------------------- */
static void u32dec(uint32_t v, char *buf, int max)
{
    char tmp[12]; int i = 0, j = 0;
    if (!v) { buf[j++] = '0'; buf[j] = '\0'; return; }
    while (v && i < 11) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i-- && j < max - 1) buf[j++] = tmp[i];
    buf[j] = '\0';
}
static void u8_2dig(uint8_t v, char *out) { out[0]=(char)('0'+v/10); out[1]=(char)('0'+v%10); }

/* -------------------------------------------------------------------------
 * Config file reader.
 * Reads the first non-empty, non-comment line from a VFS file into 'out'.
 * Returns 1 on success, 0 if file absent or no usable line.
 * ------------------------------------------------------------------------- */
static int read_first_line(const char *path, char *out, int maxlen)
{
    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ)) return 0;

    uint32_t size = VFS_Size(&fh);
    if (size == 0 || size > 4095) { VFS_Close(&fh); return 0; }

    static char buf[4096];
    uint32_t nr = VFS_Read(&fh, (uint8_t *)buf, size);
    VFS_Close(&fh);
    buf[nr] = '\0';

    const char *p = buf;
    while (*p) {
        /* Skip leading whitespace */
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        /* Skip comment lines */
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }
        /* Copy until end-of-line */
        int i = 0;
        while (*p && *p != '\r' && *p != '\n' && i < maxlen - 1)
            out[i++] = *p++;
        /* Trim trailing whitespace */
        while (i > 0 && (out[i-1] == ' ' || out[i-1] == '\t')) i--;
        out[i] = '\0';
        if (i > 0) return 1;
    }
    return 0;
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

    /* ---- Read timezone.conf ---- */
    char tz_name[64];
    if (!read_first_line(TZ_CONF_PATH, tz_name, sizeof(tz_name))) {
        int i = 0; const char *d = TZ_DEFAULT;
        while (*d && i < 63) tz_name[i++] = *d++;
        tz_name[i] = '\0';
    }
    const TzInfo *tz = tz_find(tz_name);
    if (!tz) {
        char line[128];
        cmd_scopy(line, "ntpd: unknown timezone '", sizeof(line));
        cmd_scat(line, tz_name, sizeof(line));
        cmd_scat(line, "' — using UTC", sizeof(line));
        PRINT(line);
        tz = tz_find("UTC");
    }
    tz_set_current(tz);

    /* ---- Read server from ntp.conf (or args) ---- */
    char server[256];
    if (args && *args) {
        int i = 0;
        while (*args && *args != ' ' && i < 255) server[i++] = *args++;
        server[i] = '\0';
    } else if (!read_first_line(NTP_CONF_PATH, server, sizeof(server))) {
        int i = 0; const char *d = NTP_DEFAULT_SERVER;
        while (*d && i < 255) server[i++] = *d++;
        server[i] = '\0';
    }

    /* ---- Step 1: resolve ---- */
    char line[160];
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

    /* ---- Step 2: SNTP query (returns UTC Unix seconds) ---- */
    uint32_t unix_utc = 0;
    if (!ntp_query(srv_ip, &unix_utc, 5000, ntpd_poll, ctx)) {
        PRINT("ntpd: no reply from NTP server");
        return;
    }

    /* ---- Step 3: Store UTC epoch for live ticking ---- */
    ntp_set_epoch(unix_utc);

    /* ---- Step 4: Write UTC to CMOS ---- */
    uint16_t y; uint8_t mo, d, h, mi, s;
    ntp_unix_to_datetime(unix_utc, &y, &mo, &d, &h, &mi, &s);
    RtcDateTime dt = { y, mo, d, h, mi, s };
    RTC_SetDateTime(&dt);

    /* ---- Step 5: Compute local time for display ---- */
    int32_t  off_min   = tz_offset_min(tz, unix_utc);
    uint32_t unix_local = (uint32_t)((int64_t)unix_utc + (int64_t)off_min * 60);
    uint16_t ly; uint8_t lmo, ld, lh, lmi, ls;
    ntp_unix_to_datetime(unix_local, &ly, &lmo, &ld, &lh, &lmi, &ls);
    const char *abbr = tz_abbr(tz, unix_utc);

    /* ---- Step 6: Print result ---- */
    char yr_s[8]; u32dec(ly, yr_s, sizeof(yr_s));
    char mon_s[3], day_s[3], hr_s[3], mn_s[3], sc_s[3];
    u8_2dig(lmo, mon_s); mon_s[2]='\0';
    u8_2dig(ld,  day_s); day_s[2]='\0';
    u8_2dig(lh,  hr_s);  hr_s[2] ='\0';
    u8_2dig(lmi, mn_s);  mn_s[2] ='\0';
    u8_2dig(ls,  sc_s);  sc_s[2] ='\0';

    cmd_scopy(line, "ntpd: clock set to ", sizeof(line));
    cmd_scat(line, yr_s,  sizeof(line)); cmd_scat(line, "-",  sizeof(line));
    cmd_scat(line, mon_s, sizeof(line)); cmd_scat(line, "-",  sizeof(line));
    cmd_scat(line, day_s, sizeof(line)); cmd_scat(line, " ",  sizeof(line));
    cmd_scat(line, hr_s,  sizeof(line)); cmd_scat(line, ":",  sizeof(line));
    cmd_scat(line, mn_s,  sizeof(line)); cmd_scat(line, ":",  sizeof(line));
    cmd_scat(line, sc_s,  sizeof(line)); cmd_scat(line, " ",  sizeof(line));
    cmd_scat(line, abbr,  sizeof(line));
    cmd_scat(line, " (",  sizeof(line));
    cmd_scat(line, tz_name, sizeof(line));
    cmd_scat(line, ")",   sizeof(line));
    PRINT(line);
}
