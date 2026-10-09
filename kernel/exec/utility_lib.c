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
#define UTIL_TO_UPPER       7
#define UTIL_TO_LOWER       8
#define UTIL_SMULT32        9
#define UTIL_UMULT32        10
#define UTIL_NEXT_TAG_ITEM  11
#define UTIL_GET_TAG_DATA   12
#define UTIL_DATE_MATCH     13
#define UTIL_SMULT64        14
#define UTIL_UMULT64        15
#define UTIL_FIND_TAG_ITEM  16
#define UTIL_PACK_BOOL_TAGS 17
#define UTIL_FILTER_TAG_CHANGES 18
#define UTIL_MAP_TAGS       19
#define UTIL_TAG_IN_ARRAY   20
#define UTIL_FILTER_TAG_ITEMS 21
#define UTIL_APPLY_TAG_CHANGES 22
#define UTIL_REFRESH_TAG_ITEM_CLONES 23
#define UTIL_SDIVMOD32      24
#define UTIL_UDIVMOD32      25
#define UTIL_AMIGA2DATE     26
#define UTIL_DATE2AMIGA     27
#define UTIL_CHECKDATE      28
#define UTIL_CALL_HOOK_PKT  29

/* =========================================================================
 * TagItem structure and system tag values (utility/tagitem.h)
 * Tags 0-3 are reserved; user tags have TAG_USER (bit 31) set.
 * ========================================================================= */
typedef struct TagItem {
    uint32_t ti_Tag;
    uint32_t ti_Data;
} TagItem;

#define TAG_DONE   0
#define TAG_IGNORE 1
#define TAG_MORE   2
#define TAG_SKIP   3
#define TAG_USER   0x80000000UL

/* =========================================================================
 * Character case helpers
 * The built-in tables also cover the 8-bit range: upper folds 0xE0-0xFE
 * (except 0xF7) onto 0xC0-0xDE; lower folds 0x41-0x5A and 0xC0-0xDE.
 * ========================================================================= */

static uint8_t util_to_upper(uint8_t c)
{
    if ((c >= 0x61 && c <= 0x7a) || (c >= 0xe0 && c <= 0xfe && c != 0xf7))
        return (uint8_t)(c - 0x20);
    return c;
}

static uint8_t util_to_lower(uint8_t c)
{
    if ((c >= 0x41 && c <= 0x5a) || (c >= 0xc0 && c <= 0xde))
        return (uint8_t)(c + 0x20);
    return c;
}

/* Access to guest RAM for string/tag operations
 * Note: These utility functions operate on M68k guest memory
 * The addresses in CPU registers are guest addresses that need
 * to be accessed through g_ram */
extern uint8_t *g_ram;
#define M68K_TO_HOST(addr) ((void *)(g_ram + (addr)))

extern uint32_t UAOS_InvokeM68kHook(uint32_t hook_ptr, uint32_t a0,
                                  uint32_t a1, uint32_t a2);

/* Guest memory is big-endian — read/write multi-byte words byte-wise. */
static uint32_t util_r32(uint32_t addr)
{
    if (addr >= (uint32_t)GUEST_RAM_SIZE ||
        addr + 4 > (uint32_t)GUEST_RAM_SIZE)
        return 0;
    return ((uint32_t)g_ram[addr + 0] << 24)
         | ((uint32_t)g_ram[addr + 1] << 16)
         | ((uint32_t)g_ram[addr + 2] <<  8)
         | ((uint32_t)g_ram[addr + 3]      );
}

static void util_w32(uint32_t addr, uint32_t val)
{
    if (addr >= (uint32_t)GUEST_RAM_SIZE ||
        addr + 4 > (uint32_t)GUEST_RAM_SIZE)
        return;
    g_ram[addr + 0] = (uint8_t)(val >> 24);
    g_ram[addr + 1] = (uint8_t)(val >> 16);
    g_ram[addr + 2] = (uint8_t)(val >>  8);
    g_ram[addr + 3] = (uint8_t)(val      );
}

static uint16_t util_r16(uint32_t addr)
{
    if (addr >= (uint32_t)GUEST_RAM_SIZE ||
        addr + 2 > (uint32_t)GUEST_RAM_SIZE)
        return 0;
    return (uint16_t)(((uint16_t)g_ram[addr] << 8) | g_ram[addr + 1]);
}

static void util_w16(uint32_t addr, uint16_t val)
{
    if (addr >= (uint32_t)GUEST_RAM_SIZE ||
        addr + 2 > (uint32_t)GUEST_RAM_SIZE)
        return;
    g_ram[addr]     = (uint8_t)(val >> 8);
    g_ram[addr + 1] = (uint8_t)(val     );
}

/* =========================================================================
 * Tag list walker
 *
 * Advances *cursor through a tag list with canonical NextTagItem
 * semantics: TAG_IGNORE/TAG_SKIP entries are skipped, TAG_MORE chains are
 * followed transparently, and TAG_DONE (or a NULL/bad pointer) clears
 * *cursor and yields NULL.  On a normal tag *cursor is advanced past the
 * item and the item address is returned.
 * ========================================================================= */
static uint32_t util_tag_next(uint32_t *cursor)
{
    uint32_t item = *cursor;
    int guard = 0;
    while (item && item + 8 <= (uint32_t)GUEST_RAM_SIZE && guard++ < 16384) {
        uint32_t tag = util_r32(item);
        if (tag == TAG_DONE)
            break;
        if (tag == TAG_IGNORE) { item += 8;                            continue; }
        if (tag == TAG_MORE)   { item = util_r32(item + 4);            continue; }
        if (tag == TAG_SKIP)   { item += (util_r32(item + 4) + 1) * 8; continue; }
        *cursor = item + 8;
        return item;
    }
    *cursor = 0;
    return 0;
}

static uint32_t util_tag_find(uint32_t tag, uint32_t list)
{
    uint32_t cur = list, item;
    while ((item = util_tag_next(&cur)) != 0)
        if (util_r32(item) == tag)
            return item;
    return 0;
}

static uint32_t util_tag_data(uint32_t tag, uint32_t fallback, uint32_t list)
{
    uint32_t item = util_tag_find(tag, list);
    return item ? util_r32(item + 4) : fallback;
}

static int util_tag_in_array(uint32_t tag, uint32_t array)
{
    int guard = 0;
    while (array + 4 <= (uint32_t)GUEST_RAM_SIZE && guard++ < 65536) {
        uint32_t t = util_r32(array);
        if (!t) break;
        if (t == tag) return 1;
        array += 4;
    }
    return 0;
}

/* =========================================================================
 * International string comparison
 * The result is a sign-extended 8-bit difference of the first mismatching
 * byte pair; case folding applies only when both bytes are non-zero.
 * ========================================================================= */
static int32_t util_strnicmp(uint32_t s1, uint32_t s2, int32_t length)
{
    while (length > 0) {
        if (s1 >= (uint32_t)GUEST_RAM_SIZE || s2 >= (uint32_t)GUEST_RAM_SIZE)
            break;
        uint8_t a = g_ram[s1++], b = g_ram[s2++];
        --length;
        if (a && b) {
            a = util_to_upper(a);
            b = util_to_upper(b);
        }
        uint8_t diff = (uint8_t)(a - b);
        if (diff || !a || !b)
            return diff < 128 ? (int32_t)diff : (int32_t)diff - 256;
    }
    return 0;
}

/* =========================================================================
 * Date conversion (Amiga epoch = 1-Jan-1978)
 * ClockData is seven big-endian UWORDs: sec, min, hour, mday, month,
 * year, wday.  The calendar arithmetic is March-based with a day offset
 * of 0x000b05d6 for the 1978 epoch.
 * ========================================================================= */
static void util_amiga2date(uint32_t seconds, uint16_t f[7])
{
    uint32_t days, adjusted, year, diy, month;
    f[0] = (uint16_t)(seconds % 60); seconds /= 60;
    f[1] = (uint16_t)(seconds % 60); seconds /= 60;
    f[2] = (uint16_t)(seconds % 24);
    days = seconds / 24 + 0x000b05d6u;
    f[6] = (uint16_t)((days + 3) % 7);
    adjusted = days - (days + 1) / 146097;
    adjusted += adjusted / 36524;
    adjusted -= (adjusted + 1) / 1461;
    year = adjusted / 365;
    diy = days - year / 400 + year / 100 - year / 4 - year * 365;
    month = (diy % 153) * 10 / 305 + (diy / 153) * 5;
    f[3] = (uint16_t)(diy + 1 - (month * 306 + 5) / 10);
    f[4] = (uint16_t)((month + 2) % 12 + 1);
    f[5] = (uint16_t)(year + (month + 2) / 12);
}

static uint32_t util_date2amiga(const uint16_t f[7])
{
    uint32_t month = (uint32_t)f[4] + 9;
    uint32_t year = (uint32_t)f[5] + month / 12 - 1;
    uint32_t days = year * 365 + year / 4 - year / 100 + year / 400;
    uint32_t seconds;
    days += ((month % 12) * 306 + 5) / 10 + f[3] - 1;
    days -= 0x000b05d6u;
    seconds = days * 24 + f[2];
    seconds = seconds * 60 + f[1];
    return seconds * 60 + f[0];
}

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
     * Returns: D0 = sign-extended 8-bit difference */
    cpu->d[0] = (uint32_t)util_strnicmp(cpu->a[0], cpu->a[1], 0x7fffffff);
}

static void util_StrNicmp(M68kCPUState *cpu)
{
    /* Strnicmp - case-insensitive string comparison with length
     * A0 = string1, A1 = string2, D0 = length
     * Returns: D0 = sign-extended 8-bit difference */
    cpu->d[0] = (uint32_t)util_strnicmp(cpu->a[0], cpu->a[1],
                                        (int32_t)cpu->d[0]);
}

static void util_ToUpper(M68kCPUState *cpu)
{
    /* ToUpper - fold one character to uppercase
     * D0 = character
     * Returns: D0 = folded character (low byte) */
    cpu->d[0] = util_to_upper((uint8_t)cpu->d[0]);
}

static void util_ToLower(M68kCPUState *cpu)
{
    /* ToLower - fold one character to lowercase
     * D0 = character
     * Returns: D0 = folded character (low byte) */
    cpu->d[0] = util_to_lower((uint8_t)cpu->d[0]);
}

static void util_SMult32(M68kCPUState *cpu)
{
    /* SMult32 - signed 32x32 multiply, low 32 bits of the product
     * D0 = arg1, D1 = arg2
     * Returns: D0 = arg1 * arg2 (low 32 bits) */
    cpu->d[0] = (uint32_t)((uint64_t)(int32_t)cpu->d[0] *
                          (uint64_t)(int32_t)cpu->d[1]);
}

static void util_UMult32(M68kCPUState *cpu)
{
    /* UMult32 - unsigned 32x32 multiply, low 32 bits of the product
     * D0 = arg1, D1 = arg2
     * Returns: D0 = arg1 * arg2 (low 32 bits) */
    cpu->d[0] = (uint32_t)((uint64_t)cpu->d[0] * (uint64_t)cpu->d[1]);
}

static void util_SDivMod32(M68kCPUState *cpu)
{
    /* SDivMod32 - signed 32/32 divide
     * D0 = dividend, D1 = divisor
     * Returns: D0 = quotient, D1 = remainder */
    int32_t dividend = (int32_t)cpu->d[0];
    int32_t divisor  = (int32_t)cpu->d[1];
    int32_t quot;
    if (!divisor || (dividend == (int32_t)0x80000000 && divisor == -1)) {
        cpu->d[0] = 0;
        cpu->d[1] = 0;
        return;
    }
    quot = dividend / divisor;
    cpu->d[0] = (uint32_t)quot;
    cpu->d[1] = (uint32_t)(dividend - quot * divisor);
}

static void util_UDivMod32(M68kCPUState *cpu)
{
    /* UDivMod32 - unsigned 32/32 divide
     * D0 = dividend, D1 = divisor
     * Returns: D0 = quotient, D1 = remainder */
    uint32_t quot;
    if (!cpu->d[1]) {
        cpu->d[0] = 0;
        return;
    }
    quot = cpu->d[0] / cpu->d[1];
    cpu->d[1] = cpu->d[0] - quot * cpu->d[1];
    cpu->d[0] = quot;
}

static void util_NextTagItem(M68kCPUState *cpu)
{
    /* NextTagItem - iterate through tag list
     * A0 = guest pointer to tag list pointer (updated to next item)
     * Returns: D0 = current tag item guest pointer or NULL if end.
     * Guest RAM is big-endian and returns must be guest addresses, not
     * host pointers. */
    uint32_t cur = util_r32(cpu->a[0]);
    cpu->d[0] = util_tag_next(&cur);
    util_w32(cpu->a[0], cur);
}

static void util_FindTagItem(M68kCPUState *cpu)
{
    /* FindTagItem - find a tag in a tag list
     * D0 = tag ID to search for, A0 = tag list (guest addr)
     * Returns: D0 = TagItem guest pointer or NULL */
    cpu->d[0] = util_tag_find(cpu->d[0], cpu->a[0]);
}

static void util_GetTagData(M68kCPUState *cpu)
{
    /* GetTagData - get data value for a tag from tag list
     * D0 = tag ID to search for, D1 = default, A0 = tag list (guest addr)
     * Returns: D0 = tag data value or default if not found */
    cpu->d[0] = util_tag_data(cpu->d[0], cpu->d[1], cpu->a[0]);
}

static void util_PackBoolTags(M68kCPUState *cpu)
{
    /* PackBoolTags - fold a tag list into boolean flags
     * D0 = initial flags, A0 = tag list, A1 = bool map tag list
     * Returns: D0 = updated flags */
    uint32_t flags = cpu->d[0];
    uint32_t cur = cpu->a[0], item;
    while ((item = util_tag_next(&cur)) != 0) {
        uint32_t mask = util_tag_data(util_r32(item), 0, cpu->a[1]);
        flags = util_r32(item + 4) ? (flags | mask) : (flags & ~mask);
    }
    cpu->d[0] = flags;
}

static void util_FilterTagChanges(M68kCPUState *cpu)
{
    /* FilterTagChanges - drop changes matching the original list
     * A0 = change list, A1 = original list, D0 = apply flag */
    uint32_t cur = cpu->a[0], item;
    while ((item = util_tag_next(&cur)) != 0) {
        uint32_t old = util_tag_find(util_r32(item), cpu->a[1]);
        if (!old)
            continue;
        if (util_r32(old + 4) == util_r32(item + 4))
            util_w32(item, TAG_IGNORE);
        else if (cpu->d[0])
            util_w32(old + 4, util_r32(item + 4));
    }
}

static void util_MapTags(M68kCPUState *cpu)
{
    /* MapTags - remap tag values through a map list
     * A0 = tag list, A1 = map list, D0 = map type (all 32 bits) */
    uint32_t cur = cpu->a[0], item;
    while ((item = util_tag_next(&cur)) != 0) {
        uint32_t mapping = util_tag_find(util_r32(item), cpu->a[1]);
        if (mapping) {
            uint32_t new_tag = util_r32(mapping + 4);
            util_w32(item, new_tag ? new_tag : TAG_IGNORE);
        } else if (!cpu->d[0]) {
            util_w32(item, TAG_IGNORE);
        }
    }
}

static void util_TagInArray(M68kCPUState *cpu)
{
    /* TagInArray - test whether a tag appears in a 0-terminated Tag array
     * D0 = tag, A0 = tag array
     * Returns: D0 = 1 if found, 0 otherwise */
    cpu->d[0] = (uint32_t)util_tag_in_array(cpu->d[0], cpu->a[0]);
}

static void util_FilterTagItems(M68kCPUState *cpu)
{
    /* FilterTagItems - mark tags as TAG_IGNORE per a filter array
     * A0 = tag list, A1 = filter Tag array, D0 = logic (low byte only)
     * Returns: D0 = number of items left unfiltered */
    uint32_t cur = cpu->a[0], item, remaining = 0;
    int drop_found = (cpu->d[0] & 0xff) != 0;
    while ((item = util_tag_next(&cur)) != 0) {
        if (util_tag_in_array(util_r32(item), cpu->a[1]) == drop_found)
            util_w32(item, TAG_IGNORE);
        else
            ++remaining;
    }
    cpu->d[0] = remaining;
}

static void util_ApplyTagChanges(M68kCPUState *cpu)
{
    /* ApplyTagChanges - apply data values from a change list
     * A0 = tag list, A1 = change list */
    uint32_t cur = cpu->a[0], item;
    while ((item = util_tag_next(&cur)) != 0) {
        uint32_t change = util_tag_find(util_r32(item), cpu->a[1]);
        if (change)
            util_w32(item + 4, util_r32(change + 4));
    }
}

static void util_RefreshTagItemClones(M68kCPUState *cpu)
{
    /* RefreshTagItemClones - copy a tag list over an existing clone
     * A0 = clone (destination), A1 = original list
     * Terminates the clone's tag field only; its data field is kept. */
    uint32_t dst = cpu->a[0], cur = cpu->a[1], item;
    while ((item = util_tag_next(&cur)) != 0) {
        util_w32(dst,     util_r32(item));
        util_w32(dst + 4, util_r32(item + 4));
        dst += 8;
    }
    util_w32(dst, TAG_DONE);
}

static void util_CallHookPkt(M68kCPUState *cpu)
{
    /* CallHookPkt - invoke a hook's h_Entry
     * A0 = hook, A2 = object, A1 = parameter packet
     * Returns: D0 = hook result */
    cpu->d[0] = UAOS_InvokeM68kHook(cpu->a[0], cpu->a[0],
                                  cpu->a[1], cpu->a[2]);
}

static void util_Amiga2Date(M68kCPUState *cpu)
{
    /* Amiga2Date - convert seconds since 1978 to ClockData
     * D0 = seconds, A0 = ClockData (guest addr, 14 bytes) */
    uint16_t f[7];
    util_amiga2date(cpu->d[0], f);
    for (int i = 0; i < 7; i++)
        util_w16(cpu->a[0] + (uint32_t)i * 2, f[i]);
}

static void util_Date2Amiga(M68kCPUState *cpu)
{
    /* Date2Amiga - convert ClockData to seconds since 1978
     * A0 = ClockData (guest addr)
     * Returns: D0 = seconds */
    uint16_t f[7];
    for (int i = 0; i < 7; i++)
        f[i] = util_r16(cpu->a[0] + (uint32_t)i * 2);
    cpu->d[0] = util_date2amiga(f);
}

static void util_CheckDate(M68kCPUState *cpu)
{
    /* CheckDate - validate ClockData, weekday ignored
     * A0 = ClockData (guest addr)
     * Returns: D0 = seconds since 1978, or 0 if the date is invalid */
    uint16_t f[7], n[7];
    uint32_t seconds;
    for (int i = 0; i < 7; i++)
        f[i] = util_r16(cpu->a[0] + (uint32_t)i * 2);
    seconds = util_date2amiga(f);
    util_amiga2date(seconds, n);
    if (f[0] != n[0] || f[1] != n[1] || f[2] != n[2] ||
        f[3] != n[3] || f[4] != n[4] || f[5] != n[5]) {
        cpu->d[0] = 0;
        return;
    }
    cpu->d[0] = seconds;
}

static void util_SMult64(M68kCPUState *cpu)
{
    /* SMult64 - signed 32x32->64 multiply
     * D0 = factor1, D1 = factor2
     * Returns: D0:D1 = 64-bit product (D0 high, D1 low) */
    int64_t result = (int64_t)(int32_t)cpu->d[0] * (int64_t)(int32_t)cpu->d[1];
    cpu->d[0] = (uint32_t)((uint64_t)result >> 32);
    cpu->d[1] = (uint32_t)result;
}

static void util_UMult64(M68kCPUState *cpu)
{
    /* UMult64 - unsigned 32x32->64 multiply
     * D0 = factor1, D1 = factor2
     * Returns: D0:D1 = 64-bit product (D0 high, D1 low) */
    uint64_t result = (uint64_t)cpu->d[0] * (uint64_t)cpu->d[1];
    cpu->d[0] = (uint32_t)(result >> 32);
    cpu->d[1] = (uint32_t)result;
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
    util_OpenLibrary,             /* index 1  */
    util_CloseLibrary,            /* index 2  */
    util_AllocItem,               /* index 3  */
    util_FreeItem,                /* index 4  */
    util_StrIcmp,                 /* index 5  */
    util_StrNicmp,                /* index 6  */
    util_ToUpper,                 /* index 7  */
    util_ToLower,                 /* index 8  */
    util_SMult32,                 /* index 9  */
    util_UMult32,                 /* index 10 */
    util_NextTagItem,             /* index 11 */
    util_GetTagData,              /* index 12 */
    util_DateMatch,               /* index 13 */
    util_SMult64,                 /* index 14 */
    util_UMult64,                 /* index 15 */
    util_FindTagItem,             /* index 16 */
    util_PackBoolTags,            /* index 17 */
    util_FilterTagChanges,        /* index 18 */
    util_MapTags,                 /* index 19 */
    util_TagInArray,              /* index 20 */
    util_FilterTagItems,          /* index 21 */
    util_ApplyTagChanges,         /* index 22 */
    util_RefreshTagItemClones,    /* index 23 */
    util_SDivMod32,               /* index 24 */
    util_UDivMod32,               /* index 25 */
    util_Amiga2Date,              /* index 26 */
    util_Date2Amiga,              /* index 27 */
    util_CheckDate,               /* index 28 */
    util_CallHookPkt,             /* index 29 */
};

/* Canonical utility.library LVOs -> util_funcs[] indices.
 * The -552/-414 entries keep legacy guest calls that reach for an exec-style
 * OpenLibrary/CloseLibrary through the utility base working. */
static const UaosRomLvo util_lvo_map[] = {
    {  -30, UTIL_FIND_TAG_ITEM },          /* FindTagItem          */
    {  -36, UTIL_GET_TAG_DATA },           /* GetTagData           */
    {  -42, UTIL_PACK_BOOL_TAGS },         /* PackBoolTags         */
    {  -48, UTIL_NEXT_TAG_ITEM },          /* NextTagItem          */
    {  -54, UTIL_FILTER_TAG_CHANGES },     /* FilterTagChanges     */
    {  -60, UTIL_MAP_TAGS },               /* MapTags              */
    {  -84, UTIL_REFRESH_TAG_ITEM_CLONES },/* RefreshTagItemClones */
    {  -90, UTIL_TAG_IN_ARRAY },           /* TagInArray           */
    {  -96, UTIL_FILTER_TAG_ITEMS },       /* FilterTagItems       */
    { -102, UTIL_CALL_HOOK_PKT },          /* CallHookPkt          */
    { -120, UTIL_AMIGA2DATE },             /* Amiga2Date           */
    { -126, UTIL_DATE2AMIGA },             /* Date2Amiga           */
    { -132, UTIL_CHECKDATE },              /* CheckDate            */
    { -138, UTIL_SMULT32 },                /* SMult32              */
    { -144, UTIL_UMULT32 },                /* UMult32              */
    { -150, UTIL_SDIVMOD32 },              /* SDivMod32            */
    { -156, UTIL_UDIVMOD32 },              /* UDivMod32            */
    { -162, UTIL_STR_ICMP },               /* Stricmp              */
    { -168, UTIL_STR_NICMP },              /* Strnicmp             */
    { -174, UTIL_TO_UPPER },               /* ToUpper              */
    { -180, UTIL_TO_LOWER },               /* ToLower              */
    { -186, UTIL_APPLY_TAG_CHANGES },      /* ApplyTagChanges      */
    { -198, UTIL_SMULT64 },                /* SMult64              */
    { -204, UTIL_UMULT64 },                /* UMult64              */
    { -552, UTIL_OPEN_LIBRARY },
    { -414, UTIL_CLOSE_LIBRARY },
};

/* =========================================================================
 * Registration function
 * ========================================================================= */

void UAOS_UTILITY_Register(void)
{
    UAOS_ROM_Register("utility.library", 37, 0x00000050,
                      (uint16_t)(sizeof(util_funcs) / sizeof(util_funcs[0])),
                      util_funcs);
    UAOS_ROM_BindLvoMap("utility.library", util_lvo_map,
                        (uint16_t)(sizeof(util_lvo_map) / sizeof(util_lvo_map[0])));
}
