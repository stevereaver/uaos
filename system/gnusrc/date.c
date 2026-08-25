/* date.c — GNU coreutils 'date' for UAOS gnu: layer
 *
 * Print or set the system date and time.
 *   date [OPTION]... [+FORMAT]
 * Options: -u, --utc, -d STRING, --date=STRING, -r FILE, --reference=FILE,
 *          -s STRING, --set=STRING, FORMAT specifiers: %Y %m %d %H %M %S %j %a %A %b %B %y %p %r %T %D %F
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_utc = 0;

/* Get current time — UAOS doesn't have a direct time syscall, but we can
 * stat the current directory to get a rough mtime, or use a kernel time
 * syscall if available.  For now, we use the volume info or a stat of "." */
static uint32_t get_time(void)
{
    char cwd[UAOS_CMD_PATH_MAX];
    uaos_getcwd(cwd, sizeof(cwd));
    struct uaos_stat st;
    if (uaos_stat(cwd, &st) == 0) return st.mtime;
    return 0;
}

static void format_date(uint32_t ts, const char *fmt)
{
    if (ts == 0) { put_s("Thu Jan  1 00:00:00 1970"); return; }

    static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    static const char *day = "SunMonTueWedThuFriSat";
    static const char *day_full[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
    static const char *mon_full[] = {"January","February","March","April","May","June","July","August","September","October","November","December"};

    uint32_t days = ts / 86400;
    uint32_t secs = ts % 86400;
    int hour = (int)(secs / 3600);
    int min  = (int)((secs / 60) % 60);
    int sec  = (int)(secs % 60);
    int dow  = (int)((4 + days) % 7);

    int32_t z = (int32_t)days + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t y = (int32_t)(yoe) + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y += 1;

    char buf[16];
    const char *p = fmt;
    while (*p) {
        if (*p == '%') {
            p++;
            switch (*p) {
                case 'Y':
                    uint_to_dec((uint32_t)y, buf, sizeof(buf));
                    put_s(buf);
                    break;
                case 'y':
                    uint_to_dec((uint32_t)(y % 100), buf, sizeof(buf));
                    if (uaos_strlen(buf) < 2) put_c('0');
                    put_s(buf);
                    break;
                case 'm':
                    uint_to_dec(m, buf, sizeof(buf));
                    if (uaos_strlen(buf) < 2) put_c('0');
                    put_s(buf);
                    break;
                case 'd':
                    uint_to_dec(d, buf, sizeof(buf));
                    if (uaos_strlen(buf) < 2) put_c('0');
                    put_s(buf);
                    break;
                case 'H':
                    uint_to_dec((uint32_t)hour, buf, sizeof(buf));
                    if (uaos_strlen(buf) < 2) put_c('0');
                    put_s(buf);
                    break;
                case 'M':
                    uint_to_dec((uint32_t)min, buf, sizeof(buf));
                    if (uaos_strlen(buf) < 2) put_c('0');
                    put_s(buf);
                    break;
                case 'S':
                    uint_to_dec((uint32_t)sec, buf, sizeof(buf));
                    if (uaos_strlen(buf) < 2) put_c('0');
                    put_s(buf);
                    break;
                case 'a': put_s(day + dow * 3); break;
                case 'A': put_s(day_full[dow]); break;
                case 'b': case 'h': put_s(mon + (m - 1) * 3); break;
                case 'B': put_s(mon_full[m - 1]); break;
                case 'j': {
                    uint32_t jday = doy + 1;
                    uint_to_dec(jday, buf, sizeof(buf));
                    for (int i = (int)uaos_strlen(buf); i < 3; i++) put_c('0');
                    put_s(buf);
                    break;
                }
                case 'p': put_s(hour < 12 ? "AM" : "PM"); break;
                case 'D':
                    uint_to_dec(m, buf, sizeof(buf));
                    if (uaos_strlen(buf) < 2) put_c('0');
                    put_s(buf); put_c('/');
                    uint_to_dec(d, buf, sizeof(buf));
                    if (uaos_strlen(buf) < 2) put_c('0');
                    put_s(buf); put_c('/');
                    uint_to_dec((uint32_t)(y % 100), buf, sizeof(buf));
                    if (uaos_strlen(buf) < 2) put_c('0');
                    put_s(buf);
                    break;
                case 'F':
                    uint_to_dec((uint32_t)y, buf, sizeof(buf));
                    put_s(buf); put_c('-');
                    uint_to_dec(m, buf, sizeof(buf));
                    if (uaos_strlen(buf) < 2) put_c('0');
                    put_s(buf); put_c('-');
                    uint_to_dec(d, buf, sizeof(buf));
                    if (uaos_strlen(buf) < 2) put_c('0');
                    put_s(buf);
                    break;
                case 'T':
                    uint_to_dec((uint32_t)hour, buf, sizeof(buf));
                    if (uaos_strlen(buf) < 2) put_c('0');
                    put_s(buf); put_c(':');
                    uint_to_dec((uint32_t)min, buf, sizeof(buf));
                    if (uaos_strlen(buf) < 2) put_c('0');
                    put_s(buf); put_c(':');
                    uint_to_dec((uint32_t)sec, buf, sizeof(buf));
                    if (uaos_strlen(buf) < 2) put_c('0');
                    put_s(buf);
                    break;
                case 'r': {
                    int h12 = hour % 12; if (h12 == 0) h12 = 12;
                    uint_to_dec((uint32_t)h12, buf, sizeof(buf));
                    if (uaos_strlen(buf) < 2) put_c('0');
                    put_s(buf); put_c(':');
                    uint_to_dec((uint32_t)min, buf, sizeof(buf));
                    if (uaos_strlen(buf) < 2) put_c('0');
                    put_s(buf); put_c(':');
                    uint_to_dec((uint32_t)sec, buf, sizeof(buf));
                    if (uaos_strlen(buf) < 2) put_c('0');
                    put_s(buf); put_c(' ');
                    put_s(hour < 12 ? "AM" : "PM");
                    break;
                }
                case 'n': put_c('\n'); break;
                case 't': put_c('\t'); break;
                case '%': put_c('%'); break;
                case 'Z': put_s("UTC"); break;
                case '\0': p--; break;
                default: put_c('%'); put_c(*p); break;
            }
        } else {
            put_c(*p);
        }
        p++;
    }
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"utc",     'u', no_argument},
        {"date",    'd', required_argument},
        {"reference",'r', required_argument},
        {"set",     's', required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "ud:r:s:", long_opts, &li)) != -1) {
        switch (opt) {
            case 'u': opt_utc = 1; break;
            case 'd': break; /* date string — not fully supported */
            case 'r': break; /* reference file — not fully supported */
            case 's': break; /* set — not supported */
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    const char *fmt = NULL;
    if (nops >= 1) {
        const char *arg = uaos_operand(argc, argv, 0);
        if (arg && arg[0] == '+') fmt = arg + 1;
    }

    uint32_t ts = get_time();
    if (fmt) {
        format_date(ts, fmt);
    } else {
        /* default format: "Day Mon D HH:MM:SS UTC YYYY" */
        char datebuf[20];
        cmd_fmt_mtime(ts, datebuf, sizeof(datebuf));
        /* cmd_fmt_mtime gives "DD-Mon-YYYY HH:MM" — let's use our own */
        static const char *day = "SunMonTueWedThuFriSat";
        static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
        uint32_t days = ts / 86400;
        uint32_t secs = ts % 86400;
        int hour = (int)(secs / 3600);
        int min  = (int)((secs / 60) % 60);
        int sec  = (int)(secs % 60);
        int dow  = (int)((4 + days) % 7);
        int32_t z = (int32_t)days + 719468;
        int32_t era = (z >= 0 ? z : z - 146096) / 146097;
        uint32_t doe = (uint32_t)(z - era * 146097);
        uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        int32_t y = (int32_t)(yoe) + era * 400;
        uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        uint32_t mp = (5 * doy + 2) / 153;
        uint32_t d = doy - (153 * mp + 2) / 5 + 1;
        uint32_t m = mp < 10 ? mp + 3 : mp - 9;
        if (m <= 2) y += 1;
        char buf[8];
        put_s(day + dow * 3); put_c(' ');
        put_s(mon + (m - 1) * 3); put_c(' ');
        uint_to_dec(d, buf, sizeof(buf));
        if (uaos_strlen(buf) < 2) put_c(' ');
        put_s(buf); put_c(' ');
        uint_to_dec((uint32_t)hour, buf, sizeof(buf));
        if (uaos_strlen(buf) < 2) put_c('0');
        put_s(buf); put_c(':');
        uint_to_dec((uint32_t)min, buf, sizeof(buf));
        if (uaos_strlen(buf) < 2) put_c('0');
        put_s(buf); put_c(':');
        uint_to_dec((uint32_t)sec, buf, sizeof(buf));
        if (uaos_strlen(buf) < 2) put_c('0');
        put_s(buf); put_s(" UTC ");
        uint_to_dec((uint32_t)y, buf, sizeof(buf));
        put_s(buf);
        put_c('\n');
    }
    return 0;
}
