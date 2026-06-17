/*
 * locale_lib.c — UAOS locale.library Implementation
 *
 * AmigaOS locale.library provides localization support including
 * date formatting, number formatting, and character classification.
 * This is a native implementation for UAOS with a default US/English locale.
 */

#include "rom_modules.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * NTP/RTC interface for time
 * ========================================================================= */
extern uint32_t ntp_get_epoch(void);
extern void ntp_unix_to_datetime(uint32_t unix_ts, uint16_t *year, uint8_t *month,
                                  uint8_t *day, uint8_t *hour, uint8_t *min, uint8_t *sec);

/* =========================================================================
 * AmigaOS DateStamp structure (days/minutes/ticks)
 * ========================================================================= */

typedef struct DateStamp {
    int32_t ds_Days;    /* Days since Jan 1, 1978 */
    int32_t ds_Minute;  /* Minutes within the day (0-1439) */
    int32_t ds_Tick;    /* Ticks within the minute (0-49) */
} DateStamp_t;

/* =========================================================================
 * Locale structure (simplified AmigaOS compatible)
 * ========================================================================= */

#define LOCALE_SIZE 256

typedef struct Locale {
    /* Library node header */
    uint8_t ln_Type;
    int8_t  ln_Pri;
    uint32_t ln_Name;

    /* Locale data */
    uint32_t loc_LocaleName;
    uint32_t loc_LanguageName;
    uint32_t loc_PrefLanguages;

    /* Date/Time formatting */
    uint32_t loc_ShortDateFormat;
    uint32_t loc_DateFormat;
    uint32_t loc_TimeFormat;
    uint32_t loc_ShortTimeFormat;

    /* Date components */
    uint32_t loc_ShortMonthNames[12];
    uint32_t loc_LongMonthNames[12];
    uint32_t loc_ShortDayNames[7];
    uint32_t loc_LongDayNames[7];

    /* Number formatting */
    uint8_t loc_DecimalPoint;
    uint8_t loc_ThousandSep;
    uint8_t loc_Grouping[4];

    /* Date base */
    int16_t loc_DateStampToDate;

    /* Flags */
    uint32_t loc_Flags;
} Locale_t;

/* =========================================================================
 * Static locale data (US English defaults)
 * ========================================================================= */

static const char *short_months[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char *long_months[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static const char *short_days[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *long_days[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

static const char default_short_date[] = "%d-%b-%Y";
static const char default_date[] = "%A, %B %e, %Y";
static const char default_time[] = "%I:%M:%S %p";
static const char default_short_time[] = "%I:%M %p";
static const char default_locale_name[] = "english";
static const char default_language[] = "English";

/* Static locale instance in ROM - initialized at runtime */
static Locale_t g_default_locale;

/* =========================================================================
 * Memory access helper
 * ========================================================================= */
extern uint8_t *g_ram;
#define M68K_TO_HOST(addr) ((void *)(g_ram + (addr)))

/* =========================================================================
 * locale.library function indices (must match AmigaOS LVO offsets)
 * ========================================================================= */

#define LOCALE_OPEN_LIBRARY    1
#define LOCALE_CLOSE_LIBRARY   2
#define LOCALE_OPEN_LOCALE     5
#define LOCALE_CLOSE_LOCALE    6
#define LOCALE_FORMAT_DATE     14
#define LOCALE_PARSE_DATE      15
#define LOCALE_GET_LOCALE_STR  24
#define LOCALE_IS_UPPER        28
#define LOCALE_IS_LOWER        29
#define LOCALE_IS_ALPHA        30
#define LOCALE_IS_DIGIT        31
#define LOCALE_IS_SPACE        32
#define LOCALE_IS_PUNCT        33

/* =========================================================================
 * Internal date conversion helpers
 * ========================================================================= */

/* Convert Amiga DateStamp to Unix epoch */
static uint32_t datestamp_to_unix(const DateStamp_t *ds)
{
    /* Amiga epoch is Jan 1, 1978 */
    /* Unix epoch is Jan 1, 1970 */
    /* Difference: 2922 days */
    int32_t unix_days = ds->ds_Days - 2922;

    if (unix_days < 0) return 0;

    uint32_t unix_sec = (uint32_t)unix_days * 86400;
    unix_sec += (uint32_t)ds->ds_Minute * 60;
    unix_sec += (uint32_t)ds->ds_Tick / 50;  /* Convert ticks to seconds */

    return unix_sec;
}

/* Get current datestamp */
static void get_current_datestamp(DateStamp_t *ds)
{
    uint32_t unix_ts = ntp_get_epoch();

    /* Convert Unix epoch to Amiga DateStamp */
    /* Days since Jan 1, 1978 */
    ds->ds_Days = (int32_t)(unix_ts / 86400) + 2922;
    ds->ds_Minute = (int32_t)((unix_ts % 86400) / 60);
    ds->ds_Tick = (int32_t)((unix_ts % 60) * 50);  /* Convert seconds to ticks */
}

/* =========================================================================
 * Simple sprintf helper for date formatting (no external deps)
 * ========================================================================= */

static int simple_snprintf(char *buf, int max, const char *str)
{
    int i = 0;
    while (str[i] && i < max - 1) {
        buf[i] = str[i];
        i++;
    }
    if (i < max) buf[i] = '\0';
    return i;
}

static int simple_snprintf_int(char *buf, int max, int val, int width)
{
    char tmp[12];
    int i = 0, j = 0;
    int is_neg = val < 0;
    unsigned int uval = is_neg ? -val : val;

    /* Build number backwards */
    do {
        tmp[i++] = '0' + (uval % 10);
        uval /= 10;
    } while (uval > 0);

    /* Handle negative and padding */
    int total_len = i;
    if (is_neg) total_len++;

    /* Pad with zeros if width specified */
    while (i < width) {
        tmp[i++] = '0';
        total_len++;
    }

    /* Copy to output in reverse order */
    if (is_neg) buf[j++] = '-';
    while (i > 0 && j < max - 1) {
        buf[j++] = tmp[--i];
    }
    if (j < max) buf[j] = '\0';
    return j;
}

/* =========================================================================
 * Format string parser for FormatDate
 * ========================================================================= */

static void format_date_string(const char *format, const DateStamp_t *ds,
                                char *buffer, int32_t max_len,
                                const Locale_t *locale)
{
    uint32_t unix_ts = datestamp_to_unix(ds);
    uint16_t year;
    uint8_t month, day, hour, minute, sec;

    ntp_unix_to_datetime(unix_ts, &year, &month, &day, &hour, &minute, &sec);

    /* Calculate day of week (0=Sunday) */
    /* Using Zeller's congruence or similar algorithm */
    int a = (14 - month) / 12;
    int y = year - a;
    int m = month + 12 * a - 2;
    int dow = (day + y + y/4 - y/100 + y/400 + (31*m)/12) % 7;
    if (dow < 0) dow += 7;

    int buf_pos = 0;
    int is_pm = (hour >= 12);
    int hour12 = hour % 12;
    if (hour12 == 0) hour12 = 12;

    while (*format && buf_pos < max_len - 1) {
        if (*format == '%' && *(format + 1)) {
            format++;
            int len;
            switch (*format) {
                case 'a':  /* Short weekday name */
                    buf_pos += simple_snprintf(buffer + buf_pos,
                                   max_len - buf_pos, short_days[dow]);
                    break;

                case 'A':  /* Long weekday name */
                    buf_pos += simple_snprintf(buffer + buf_pos,
                                   max_len - buf_pos, long_days[dow]);
                    break;

                case 'b':  /* Short month name */
                case 'h':
                    buf_pos += simple_snprintf(buffer + buf_pos,
                                   max_len - buf_pos, short_months[month - 1]);
                    break;

                case 'B':  /* Long month name */
                    buf_pos += simple_snprintf(buffer + buf_pos,
                                   max_len - buf_pos, long_months[month - 1]);
                    break;

                case 'd':  /* Day of month (01-31) */
                    buf_pos += simple_snprintf_int(buffer + buf_pos,
                                   max_len - buf_pos, day, 2);
                    break;

                case 'e':  /* Day of month (1-31) */
                    buf_pos += simple_snprintf_int(buffer + buf_pos,
                                   max_len - buf_pos, day, 0);
                    break;

                case 'H':  /* Hour (00-23) */
                    buf_pos += simple_snprintf_int(buffer + buf_pos,
                                   max_len - buf_pos, hour, 2);
                    break;

                case 'I':  /* Hour (01-12) */
                    buf_pos += simple_snprintf_int(buffer + buf_pos,
                                   max_len - buf_pos, hour12, 2);
                    break;

                case 'm':  /* Month (01-12) */
                    buf_pos += simple_snprintf_int(buffer + buf_pos,
                                   max_len - buf_pos, month, 2);
                    break;

                case 'M':  /* Minute (00-59) */
                    buf_pos += simple_snprintf_int(buffer + buf_pos,
                                   max_len - buf_pos, minute, 2);
                    break;

                case 'p':  /* AM/PM */
                    buf_pos += simple_snprintf(buffer + buf_pos,
                                   max_len - buf_pos, is_pm ? "PM" : "AM");
                    break;

                case 'S':  /* Second (00-59) */
                    buf_pos += simple_snprintf_int(buffer + buf_pos,
                                   max_len - buf_pos, sec, 2);
                    break;

                case 'y':  /* Year (00-99) */
                    buf_pos += simple_snprintf_int(buffer + buf_pos,
                                   max_len - buf_pos, year % 100, 2);
                    break;

                case 'Y':  /* Year (1970+) */
                    buf_pos += simple_snprintf_int(buffer + buf_pos,
                                   max_len - buf_pos, year, 0);
                    break;

                case '%':  /* Literal % */
                    buffer[buf_pos++] = '%';
                    break;

                default:
                    buffer[buf_pos++] = *format;
                    break;
            }
        } else {
            buffer[buf_pos++] = *format;
        }
        format++;
    }

    buffer[buf_pos] = '\0';
}

/* =========================================================================
 * locale.library function implementations
 * ========================================================================= */

static void locale_OpenLibrary(M68kCPUState *cpu)
{
    cpu->d[0] = 0x000000B0;  /* locale.library base address */
}

static void locale_CloseLibrary(M68kCPUState *cpu)
{
    (void)cpu;
}

static void locale_OpenLocale(M68kCPUState *cpu)
{
    /* OpenLocale - open a locale
     * A0 = locale name (NULL for default), D0 = tag list
     * Returns: A0 = locale pointer or NULL, D0 = 0 for success */
    uint32_t name_ptr = cpu->a[0];

    if (name_ptr == 0) {
        /* Return default locale */
        cpu->a[0] = (uint32_t)(uintptr_t)&g_default_locale;
        cpu->d[0] = 0;
        return;
    }

    /* Only support "english" locale for now */
    const char *name = (const char *)M68K_TO_HOST(name_ptr);
    /* Case-insensitive compare manually */
    int is_english = 1, is_c = 1;
    const char *p = name;
    const char *eng = "english";
    const char *c_str = "c";
    int i = 0;
    while (eng[i] && p[i]) {
        char c1 = p[i]; if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        char c2 = eng[i]; if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) { is_english = 0; break; }
        i++;
    }
    if (eng[i] || p[i]) is_english = 0;

    i = 0;
    p = name;
    while (c_str[i] && p[i]) {
        char c1 = p[i]; if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        char c2 = c_str[i]; if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) { is_c = 0; break; }
        i++;
    }
    if (c_str[i] || p[i]) is_c = 0;

    if (is_english || is_c || name[0] == '\0') {
        cpu->a[0] = (uint32_t)(uintptr_t)&g_default_locale;
        cpu->d[0] = 0;
    } else {
        cpu->a[0] = 0;
        cpu->d[0] = (uint32_t)-1;  /* Locale not found */
    }
}

static void locale_CloseLocale(M68kCPUState *cpu)
{
    /* CloseLocale - close a locale
     * A0 = locale pointer
     * For now, just a no-op since we use static locale */
    (void)cpu;
}

static void locale_FormatDate(M68kCPUState *cpu)
{
    /* FormatDate - format a date using locale format string
     * A0 = locale, A1 = format string, A2 = DateStamp, A3 = output buffer
     * D0 = max length
     * Returns: D0 = 1 on success, 0 on failure */
    uint32_t locale_ptr = cpu->a[0];
    uint32_t format_ptr = cpu->a[1];
    uint32_t datestamp_ptr = cpu->a[2];
    uint32_t buffer_ptr = cpu->a[3];
    int32_t max_len = (int32_t)cpu->d[0];

    if (!format_ptr || !buffer_ptr) {
        cpu->d[0] = 0;
        return;
    }

    const Locale_t *locale = locale_ptr ?
        (const Locale_t *)M68K_TO_HOST(locale_ptr) : &g_default_locale;

    const char *format = (const char *)M68K_TO_HOST(format_ptr);
    char *buffer = (char *)M68K_TO_HOST(buffer_ptr);

    DateStamp_t ds;
    if (datestamp_ptr) {
        const DateStamp_t *src = (const DateStamp_t *)M68K_TO_HOST(datestamp_ptr);
        ds = *src;
    } else {
        get_current_datestamp(&ds);
    }

    format_date_string(format, &ds, buffer, max_len, locale);
    cpu->d[0] = 1;
}

static void locale_ParseDate(M68kCPUState *cpu)
{
    /* ParseDate - parse a date string
     * A0 = locale, A1 = DateStamp to fill, A2 = format string,
     * A3 = date string, A4 = current datestamp
     * Returns: D0 = 1 if parsed successfully, 0 otherwise */
    uint32_t datestamp_ptr = cpu->a[1];

    /* Not fully implemented - return failure for now */
    cpu->d[0] = 0;
    (void)datestamp_ptr;
}

static void locale_GetLocaleStr(M68kCPUState *cpu)
{
    /* GetLocaleStr - get a localized string
     * D0 = string ID
     * Returns: A0 = string pointer */
    uint32_t str_id = cpu->d[0];

    /* Return default strings based on ID */
    switch (str_id) {
        case 0:  /* S_MON_SUNDAY - Short Sunday */
            cpu->a[0] = (uint32_t)(uintptr_t)short_days[0];
            break;
        case 1:  /* S_MON_MONDAY */
            cpu->a[0] = (uint32_t)(uintptr_t)short_days[1];
            break;
        case 2:  /* S_MON_TUESDAY */
            cpu->a[0] = (uint32_t)(uintptr_t)short_days[2];
            break;
        case 3:  /* S_MON_WEDNESDAY */
            cpu->a[0] = (uint32_t)(uintptr_t)short_days[3];
            break;
        case 4:  /* S_MON_THURSDAY */
            cpu->a[0] = (uint32_t)(uintptr_t)short_days[4];
            break;
        case 5:  /* S_MON_FRIDAY */
            cpu->a[0] = (uint32_t)(uintptr_t)short_days[5];
            break;
        case 6:  /* S_MON_SATURDAY */
            cpu->a[0] = (uint32_t)(uintptr_t)short_days[6];
            break;
        case 7:  /* S_DAY_1 - Short January */
            cpu->a[0] = (uint32_t)(uintptr_t)short_months[0];
            break;
        /* ... more month strings ... */
        case 18:  /* S_DAY_DEC */
            cpu->a[0] = (uint32_t)(uintptr_t)short_months[11];
            break;
        default:
            cpu->a[0] = (uint32_t)(uintptr_t)"";
            break;
    }
}

static void locale_IsUpper(M68kCPUState *cpu)
{
    /* IsUpper - test if character is uppercase
     * D0 = character
     * Returns: D0 = 1 if uppercase, 0 otherwise */
    uint8_t c = (uint8_t)(cpu->d[0] & 0xFF);
    cpu->d[0] = (c >= 'A' && c <= 'Z') ? 1 : 0;
}

static void locale_IsLower(M68kCPUState *cpu)
{
    /* IsLower - test if character is lowercase
     * D0 = character
     * Returns: D0 = 1 if lowercase, 0 otherwise */
    uint8_t c = (uint8_t)(cpu->d[0] & 0xFF);
    cpu->d[0] = (c >= 'a' && c <= 'z') ? 1 : 0;
}

static void locale_IsAlpha(M68kCPUState *cpu)
{
    /* IsAlpha - test if character is alphabetic
     * D0 = character
     * Returns: D0 = 1 if alphabetic, 0 otherwise */
    uint8_t c = (uint8_t)(cpu->d[0] & 0xFF);
    cpu->d[0] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) ? 1 : 0;
}

static void locale_IsDigit(M68kCPUState *cpu)
{
    /* IsDigit - test if character is a digit
     * D0 = character
     * Returns: D0 = 1 if digit, 0 otherwise */
    uint8_t c = (uint8_t)(cpu->d[0] & 0xFF);
    cpu->d[0] = (c >= '0' && c <= '9') ? 1 : 0;
}

static void locale_IsSpace(M68kCPUState *cpu)
{
    /* IsSpace - test if character is whitespace
     * D0 = character
     * Returns: D0 = 1 if whitespace, 0 otherwise */
    uint8_t c = (uint8_t)(cpu->d[0] & 0xFF);
    cpu->d[0] = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') ? 1 : 0;
}

static void locale_IsPunct(M68kCPUState *cpu)
{
    /* IsPunct - test if character is punctuation
     * D0 = character
     * Returns: D0 = 1 if punctuation, 0 otherwise */
    uint8_t c = (uint8_t)(cpu->d[0] & 0xFF);
    int is_punct = 0;
    if ((c >= 33 && c <= 47) || (c >= 58 && c <= 64) ||
        (c >= 91 && c <= 96) || (c >= 123 && c <= 126)) {
        is_punct = 1;
    }
    cpu->d[0] = is_punct;
}

/* =========================================================================
 * Function table
 * ========================================================================= */

static void *locale_funcs[] = {
    locale_OpenLibrary,    /* index 1  */
    locale_CloseLibrary,   /* index 2  */
    NULL,                  /* index 3  */
    NULL,                  /* index 4  */
    locale_OpenLocale,     /* index 5  */
    locale_CloseLocale,    /* index 6  */
    NULL,                  /* index 7  */
    NULL,                  /* index 8  */
    NULL,                  /* index 9  */
    NULL,                  /* index 10 */
    NULL,                  /* index 11 */
    NULL,                  /* index 12 */
    NULL,                  /* index 13 */
    locale_FormatDate,     /* index 14 */
    locale_ParseDate,      /* index 15 */
    NULL,                  /* index 16 */
    NULL,                  /* index 17 */
    NULL,                  /* index 18 */
    NULL,                  /* index 19 */
    NULL,                  /* index 20 */
    NULL,                  /* index 21 */
    NULL,                  /* index 22 */
    NULL,                  /* index 23 */
    locale_GetLocaleStr,   /* index 24 */
    NULL,                  /* index 25 */
    NULL,                  /* index 26 */
    NULL,                  /* index 27 */
    locale_IsUpper,        /* index 28 */
    locale_IsLower,        /* index 29 */
    locale_IsAlpha,        /* index 30 */
    locale_IsDigit,        /* index 31 */
    locale_IsSpace,        /* index 32 */
    locale_IsPunct,        /* index 33 */
};

/* =========================================================================
 * Registration function
 * ========================================================================= */

/* Initialize the locale structure at runtime */
static void locale_init(void)
{
    g_default_locale.ln_Type = 9;  /* NT_LIBRARY */
    g_default_locale.ln_Pri = 0;
    g_default_locale.ln_Name = 0;
    g_default_locale.loc_LocaleName = (uint32_t)(uintptr_t)default_locale_name;
    g_default_locale.loc_LanguageName = (uint32_t)(uintptr_t)default_language;
    g_default_locale.loc_PrefLanguages = 0;
    g_default_locale.loc_ShortDateFormat = (uint32_t)(uintptr_t)default_short_date;
    g_default_locale.loc_DateFormat = (uint32_t)(uintptr_t)default_date;
    g_default_locale.loc_TimeFormat = (uint32_t)(uintptr_t)default_time;
    g_default_locale.loc_ShortTimeFormat = (uint32_t)(uintptr_t)default_short_time;
    g_default_locale.loc_DecimalPoint = '.';
    g_default_locale.loc_ThousandSep = ',';
    g_default_locale.loc_Grouping[0] = 3;
    g_default_locale.loc_Grouping[1] = 3;
    g_default_locale.loc_Grouping[2] = 0;
    g_default_locale.loc_Grouping[3] = 0;
    g_default_locale.loc_DateStampToDate = 2922;
    g_default_locale.loc_Flags = 0;
}

void UAOS_LOCALE_Register(void)
{
    locale_init();
    UAOS_ROM_Register("locale.library", 38, 0x000000B0,
                      (uint16_t)(sizeof(locale_funcs) / sizeof(locale_funcs[0])),
                      locale_funcs);
}
