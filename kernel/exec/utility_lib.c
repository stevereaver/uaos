/*
 * utility_lib.c — UAOS utility.library Implementation
 *
 * AmigaOS utility.library provides string functions, memory utilities,
 * and tag list parsing. This is a native implementation for UAOS.
 */

#include "rom_modules.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * utility.library function indices (must match AmigaOS LVO offsets)
 * ========================================================================= */

#define UTIL_OPEN_LIBRARY   1
#define UTIL_CLOSE_LIBRARY  2
#define UTIL_ALLOC_ITEM     3
#define UTIL_FREE_ITEM      4
#define UTIL_STR_ICMP       5
#define UTIL_STR_NICMP      6
#define UTIL_UC_STR         7
#define UTIL_LC_STR         8
#define UTIL_SMULT32        9
#define UTIL_UMULT32        10
#define UTIL_NEXT_TAG_ITEM  11
#define UTIL_GET_TAG_DATA   12
#define UTIL_DATE_MATCH     13

/* =========================================================================
 * String Helper Functions
 * ========================================================================= */

static char to_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c + 32);
    return c;
}

static char to_upper(char c)
{
    if (c >= 'a' && c <= 'z')
        return (char)(c - 32);
    return c;
}

/* =========================================================================
 * Stub implementations
 * ========================================================================= */

static void util_OpenLibrary(void)
{
    /* OpenLibrary - return library base */
    fprintf(stderr, "[UTILITY] OpenLibrary called\n");
}

static void util_CloseLibrary(void)
{
    /* CloseLibrary - no-op for ROM library */
    fprintf(stderr, "[UTILITY] CloseLibrary called\n");
}

static void util_AllocItem(void)
{
    /* AllocateItem - allocate memory for tagged item */
    fprintf(stderr, "[UTILITY] AllocateItem called\n");
}

static void util_FreeItem(void)
{
    /* FreeItem - free memory from AllocateItem */
    fprintf(stderr, "[UTILITY] FreeItem called\n");
}

static void util_StrIcmp(void)
{
    /* Stricmp - case-insensitive string comparison
     * D1 = string1, D2 = string2
     * Returns: <0 if s1 < s2, 0 if equal, >0 if s1 > s2 */
    fprintf(stderr, "[UTILITY] StrIcmp called\n");
    /* TODO: Implement with M68k memory access */
    /* Implementation would be:
     * while (*s1 && *s2) {
     *     char c1 = to_lower(*s1++);
     *     char c2 = to_lower(*s2++);
     *     if (c1 != c2) return c1 - c2;
     * }
     * return *s1 - *s2;
     */
}

static void util_StrNicmp(void)
{
    /* Strnicmp - case-insensitive string comparison with length
     * D1 = string1, D2 = string2, D3 = length
     * Returns: <0 if s1 < s2, 0 if equal, >0 if s1 > s2 */
    fprintf(stderr, "[UTILITY] StrNicmp called\n");
    /* TODO: Implement with M68k memory access */
    /* Implementation would be similar to StrIcmp but with length limit */
}

static void util_UcStr(void)
{
    /* UcStr - convert string to uppercase in-place
     * D1 = string pointer
     * Returns: same string pointer */
    fprintf(stderr, "[UTILITY] UcStr called\n");
    /* TODO: Implement with M68k memory access */
    /* Implementation would be:
     * char *s = str;
     * while (*s) {
     *     *s = to_upper(*s);
     *     s++;
     * }
     * return str;
     */
}

static void util_LcStr(void)
{
    /* LcStr - convert string to lowercase in-place
     * D1 = string pointer
     * Returns: same string pointer */
    fprintf(stderr, "[UTILITY] LcStr called\n");
    /* TODO: Implement with M68k memory access */
    /* Implementation would be:
     * char *s = str;
     * while (*s) {
     *     *s = to_lower(*s);
     *     s++;
     * }
     * return str;
     */
}

static void util_SMult32(void)
{
    /* SMult32 - signed 32-bit multiplication */
    fprintf(stderr, "[UTILITY] SMult32 called\n");
}

static void util_UMult32(void)
{
    /* UMult32 - unsigned 32-bit multiplication */
    fprintf(stderr, "[UTILITY] UMult32 called\n");
}

static void util_NextTagItem(void)
{
    /* NextTagItem - iterate through tag list */
    fprintf(stderr, "[UTILITY] NextTagItem called\n");
}

static void util_GetTagData(void)
{
    /* GetTagData - get data from tag list */
    fprintf(stderr, "[UTILITY] GetTagData called\n");
}

static void util_DateMatch(void)
{
    /* DateMatch - compare date patterns */
    fprintf(stderr, "[UTILITY] DateMatch called\n");
}

/* =========================================================================
 * Function table
 * ========================================================================= */

static void *util_funcs[] = {
    util_OpenLibrary,   /* index 1  */
    util_CloseLibrary,  /* index 2  */
    util_AllocItem,     /* index 3  */
    util_FreeItem,      /* index 4  */
    util_StrIcmp,       /* index 5  */
    util_StrNicmp,      /* index 6  */
    util_UcStr,         /* index 7  */
    util_LcStr,         /* index 8  */
    util_SMult32,       /* index 9  */
    util_UMult32,       /* index 10 */
    util_NextTagItem,   /* index 11 */
    util_GetTagData,    /* index 12 */
    util_DateMatch,     /* index 13 */
};

/* =========================================================================
 * Registration function
 * ========================================================================= */

void UAOS_UTILITY_Register(void)
{
    UAOS_ROM_Register("utility.library", 37, 0x00000050,
                      (uint16_t)(sizeof(util_funcs) / sizeof(util_funcs[0])),
                      util_funcs);
}
