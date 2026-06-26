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
#include "../../emulation/uaos_emu.h"

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
 * TagItem structure (AmigaOS compatible)
 * ========================================================================= */
typedef struct TagItem {
    uint32_t ti_Tag;
    uint32_t ti_Data;
} TagItem;

#define TAG_DONE 0
#define TAG_MORE 0x80000001
#define TAG_IGNORE 0x80000002
#define TAG_JUMP 0x80000003
#define TAG_END 0x80000004

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

/* Access to guest RAM for string operations
 * Note: These utility functions operate on M68k guest memory
 * The addresses in CPU registers are guest addresses that need
 * to be accessed through g_ram */
extern uint8_t *g_ram;
#define M68K_TO_HOST(addr) ((void *)(g_ram + (addr)))

/* =========================================================================
 * utility.library function implementations
 * ========================================================================= */

static void util_OpenLibrary(M68kCPUState *cpu)
{
    /* OpenLibrary - return library base (already registered at 0x50) */
    cpu->d[0] = 0x00000050;
}

static void util_CloseLibrary(M68kCPUState *cpu)
{
    /* CloseLibrary - no-op for ROM library */
    (void)cpu;
}

static void util_AllocItem(M68kCPUState *cpu)
{
    /* AllocateItem - allocate memory for tagged item */
    /* For now, return 0 (not implemented) */
    cpu->d[0] = 0;
}

static void util_FreeItem(M68kCPUState *cpu)
{
    /* FreeItem - free memory from AllocateItem */
    (void)cpu;
}

static void util_StrIcmp(M68kCPUState *cpu)
{
    /* Stricmp - case-insensitive string comparison
     * A0 = string1, A1 = string2
     * Returns: D0 = <0 if s1 < s2, 0 if equal, >0 if s1 > s2 */
    const char *s1 = (const char *)M68K_TO_HOST(cpu->a[0]);
    const char *s2 = (const char *)M68K_TO_HOST(cpu->a[1]);

    while (*s1 && *s2) {
        char c1 = to_lower(*s1++);
        char c2 = to_lower(*s2++);
        if (c1 != c2) {
            cpu->d[0] = (int32_t)(c1 - c2);
            return;
        }
    }
    cpu->d[0] = (int32_t)(to_lower(*s1) - to_lower(*s2));
}

static void util_StrNicmp(M68kCPUState *cpu)
{
    /* Strnicmp - case-insensitive string comparison with length
     * A0 = string1, A1 = string2, D0 = length
     * Returns: D0 = <0 if s1 < s2, 0 if equal, >0 if s1 > s2 */
    const char *s1 = (const char *)M68K_TO_HOST(cpu->a[0]);
    const char *s2 = (const char *)M68K_TO_HOST(cpu->a[1]);
    uint32_t len = cpu->d[0];

    for (uint32_t i = 0; i < len; i++) {
        char c1 = to_lower(s1[i]);
        char c2 = to_lower(s2[i]);
        if (c1 != c2) {
            cpu->d[0] = (int32_t)(c1 - c2);
            return;
        }
        if (s1[i] == '\0' || s2[i] == '\0')
            break;
    }
    cpu->d[0] = 0;
}

static void util_UcStr(M68kCPUState *cpu)
{
    /* UCStr - convert string to uppercase in-place
     * A0 = string pointer
     * Returns: A0 = same string pointer */
    char *s = (char *)M68K_TO_HOST(cpu->a[0]);
    char *start = s;
    while (*s) {
        *s = to_upper(*s);
        s++;
    }
    cpu->a[0] = (uint32_t)(uintptr_t)start;
}

static void util_LcStr(M68kCPUState *cpu)
{
    /* LCStr - convert string to lowercase in-place
     * A0 = string pointer
     * Returns: A0 = same string pointer */
    char *s = (char *)M68K_TO_HOST(cpu->a[0]);
    char *start = s;
    while (*s) {
        *s = to_lower(*s);
        s++;
    }
    cpu->a[0] = (uint32_t)(uintptr_t)start;
}

static void util_SMult32(M68kCPUState *cpu)
{
    /* SMult32 - signed 32x32/32 multiply-divide (SmulDiv)
     * D0 = multiplicand, D1 = multiplier, D2 = divisor
     * Returns: D0 = (multiplicand * multiplier) / divisor */
    int32_t multiplicand = (int32_t)cpu->d[0];
    int32_t multiplier = (int32_t)cpu->d[1];
    int32_t divisor = (int32_t)cpu->d[2];

    if (divisor == 0) {
        cpu->d[0] = 0;
        return;
    }
    int64_t result = (int64_t)multiplicand * (int64_t)multiplier;
    cpu->d[0] = (uint32_t)(result / divisor);
}

static void util_UMult32(M68kCPUState *cpu)
{
    /* UMult32 - unsigned 32x32/32 multiply-divide
     * D0 = multiplicand, D1 = multiplier, D2 = divisor
     * Returns: D0 = (multiplicand * multiplier) / divisor */
    uint64_t multiplicand = cpu->d[0];
    uint64_t multiplier = cpu->d[1];
    uint32_t divisor = cpu->d[2];

    if (divisor == 0) {
        cpu->d[0] = 0;
        return;
    }
    cpu->d[0] = (uint32_t)((multiplicand * multiplier) / divisor);
}

static void util_NextTagItem(M68kCPUState *cpu)
{
    /* NextTagItem - iterate through tag list
     * A0 = pointer to tag list pointer (updated to next item)
     * Returns: D0 = current tag item pointer or NULL if end */
    TagItem **tag_list_ptr = (TagItem **)M68K_TO_HOST(cpu->a[0]);
    TagItem *current = *tag_list_ptr;

    if (!current) {
        cpu->d[0] = 0;
        return;
    }

    TagItem *item = (TagItem *)M68K_TO_HOST((uint32_t)(uintptr_t)current);
    uint32_t tag = item->ti_Tag;

    /* Handle special tag control codes */
    switch (tag) {
        case TAG_DONE:
        case TAG_END:
            *tag_list_ptr = NULL;
            cpu->d[0] = 0;
            return;

        case TAG_MORE:
            /* Jump to new tag list */
            *tag_list_ptr = (TagItem *)M68K_TO_HOST(item->ti_Data);
            cpu->d[0] = cpu->a[0];
            return;

        case TAG_JUMP:
            /* Jump to absolute address */
            *tag_list_ptr = (TagItem *)M68K_TO_HOST(item->ti_Data);
            cpu->d[0] = cpu->a[0];
            return;

        case TAG_IGNORE:
            /* Skip this item */
            *tag_list_ptr = current + 1;
            cpu->d[0] = (uint32_t)(uintptr_t)current;
            return;

        default:
            /* Normal tag - advance to next and return current */
            *tag_list_ptr = current + 1;
            cpu->d[0] = (uint32_t)(uintptr_t)current;
            return;
    }
}

static void util_GetTagData(M68kCPUState *cpu)
{
    /* GetTagData - get data value for a tag from tag list
     * D0 = tag ID to search for, A0 = tag list, D1 = default value
     * Returns: D0 = tag data value or default if not found */
    uint32_t tag_id = cpu->d[0];
    TagItem *tag_list = (TagItem *)M68K_TO_HOST(cpu->a[0]);
    uint32_t default_val = cpu->d[1];

    if (!tag_list) {
        cpu->d[0] = default_val;
        return;
    }

    while (1) {
        uint32_t tag = tag_list->ti_Tag;

        if (tag == TAG_DONE || tag == TAG_END) {
            break;
        }

        if (tag == tag_id) {
            cpu->d[0] = tag_list->ti_Data;
            return;
        }

        if (tag == TAG_MORE || tag == TAG_JUMP) {
            tag_list = (TagItem *)M68K_TO_HOST(tag_list->ti_Data);
            continue;
        }

        tag_list++;
    }

    cpu->d[0] = default_val;
}

static void util_DateMatch(M68kCPUState *cpu)
{
    /* DateMatch - compare date patterns (not fully implemented) */
    (void)cpu;
    cpu->d[0] = 0;  /* No match */
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
