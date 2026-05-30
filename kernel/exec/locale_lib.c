/*
 * locale_lib.c — UAOS locale.library Implementation
 *
 * AmigaOS locale.library provides localization support including
 * character encoding, date/time formatting, and string comparison.
 * This is a native implementation for UAOS with basic stubs.
 */

#include "rom_modules.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * locale.library function indices (must match AmigaOS LVO offsets)
 * ========================================================================= */

#define LOCALE_OPEN_LIBRARY   1
#define LOCALE_CLOSE_LIBRARY  2
#define LOCALE_OPEN_LOCALE    3
#define LOCALE_CLOSE_LOCALE   4
#define LOCALE_GET_LOCALE_STR 5
#define LOCALE_STR_CONV       6
#define LOCALE_STR_CONVERT    7
#define LOCALE_UPPER_CASE     8
#define LOCALE_LOWER_CASE     9
#define LOCALE_STR_ICMP       10
#define LOCALE_STR_NICMP      11
#define LOCALE_PARSE_DATE     12
#define LOCALE_FORMAT_DATE    13
#define LOCALE_GET_FORMAT_STR 14

/* =========================================================================
 * Stub implementations
 * ========================================================================= */

static void locale_OpenLibrary(void)
{
    /* OpenLibrary - return library base */
    fprintf(stderr, "[LOCALE] OpenLibrary called\n");
}

static void locale_CloseLibrary(void)
{
    /* CloseLibrary - no-op for ROM library */
    fprintf(stderr, "[LOCALE] CloseLibrary called\n");
}

static void locale_OpenLocale(void)
{
    /* OpenLocale - open a locale for a specific language */
    fprintf(stderr, "[LOCALE] OpenLocale called\n");
}

static void locale_CloseLocale(void)
{
    /* CloseLocale - close a locale */
    fprintf(stderr, "[LOCALE] CloseLocale called\n");
}

static void locale_GetLocaleStr(void)
{
    /* GetLocaleStr - get localized string */
    fprintf(stderr, "[LOCALE] GetLocaleStr called\n");
}

static void locale_StrConv(void)
{
    /* StrConv - convert string between character sets */
    fprintf(stderr, "[LOCALE] StrConv called\n");
}

static void locale_StrConvert(void)
{
    /* StrConvert - convert string with buffer */
    fprintf(stderr, "[LOCALE] StrConvert called\n");
}

static void locale_UpperCase(void)
{
    /* UpperCase - convert string to uppercase (locale-aware) */
    fprintf(stderr, "[LOCALE] UpperCase called\n");
}

static void locale_LowerCase(void)
{
    /* LowerCase - convert string to lowercase (locale-aware) */
    fprintf(stderr, "[LOCALE] LowerCase called\n");
}

static void locale_StrIcmp(void)
{
    /* StrIcmp - case-insensitive string comparison (locale-aware) */
    fprintf(stderr, "[LOCALE] StrIcmp called\n");
}

static void locale_StrNicmp(void)
{
    /* StrNicmp - case-insensitive string comparison with length (locale-aware) */
    fprintf(stderr, "[LOCALE] StrNicmp called\n");
}

static void locale_ParseDate(void)
{
    /* ParseDate - parse date string according to locale */
    fprintf(stderr, "[LOCALE] ParseDate called\n");
}

static void locale_FormatDate(void)
{
    /* FormatDate - format date according to locale */
    fprintf(stderr, "[LOCALE] FormatDate called\n");
}

static void locale_GetFormatStr(void)
{
    /* GetFormatStr - get format string for date/time */
    fprintf(stderr, "[LOCALE] GetFormatStr called\n");
}

/* =========================================================================
 * Function table
 * ========================================================================= */

static void *locale_funcs[] = {
    locale_OpenLibrary,   /* index 1  */
    locale_CloseLibrary,  /* index 2  */
    locale_OpenLocale,    /* index 3  */
    locale_CloseLocale,   /* index 4  */
    locale_GetLocaleStr,  /* index 5  */
    locale_StrConv,       /* index 6  */
    locale_StrConvert,    /* index 7  */
    locale_UpperCase,     /* index 8  */
    locale_LowerCase,     /* index 9  */
    locale_StrIcmp,       /* index 10 */
    locale_StrNicmp,      /* index 11 */
    locale_ParseDate,     /* index 12 */
    locale_FormatDate,    /* index 13 */
    locale_GetFormatStr,  /* index 14 */
};

/* =========================================================================
 * Registration function
 * ========================================================================= */

void UAOS_LOCALE_Register(void)
{
    UAOS_ROM_Register("locale.library", 38, 0x00000080,
                      (uint16_t)(sizeof(locale_funcs) / sizeof(locale_funcs[0])),
                      locale_funcs);
}
