/*
 * graphics_lib.c — UAOS graphics.library Implementation
 *
 * AmigaOS graphics.library provides basic graphics primitives including
 * text rendering, shapes, bitmaps, and RastPort operations. This is a
 * native implementation for UAOS using the existing framebuffer driver.
 *
 * All drawing functions read their arguments from the Musashi m68k
 * register file and write directly to the linear framebuffer.
 */

#include "rom_modules.h"
#include "amiga_graphics.h"
#include "intuition_lib.h"
#include "chipset/chip_emu.h"
#include "../display/framebuffer.h"
#include "../display/wm.h"
#include "../boot/kprint.h"
#include "task.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* g_wait_tof_task — defined here (in graphics_lib.c) rather than in task.c
 * to avoid shifting Task_SwitchPrev/Task_SwitchNext in BSS, which for reasons
 * not yet understood causes a system-wide freeze.  Declared extern in task.h. */
UaosTask *g_wait_tof_task = NULL;

/* =========================================================================
 * Musashi register access (provided by emulation/uaos_m68k_glue.c)
 * ========================================================================= */

extern unsigned int m68k_get_reg(void *context, int reg);
extern void         m68k_set_reg(int reg, unsigned int value);
extern unsigned int m68k_read_memory_8(unsigned int addr);
extern unsigned int m68k_read_memory_16(unsigned int addr);
extern unsigned int m68k_read_memory_32(unsigned int addr);
extern void         m68k_write_memory_8(unsigned int addr, unsigned int val);
extern void         m68k_write_memory_16(unsigned int addr, unsigned int val);
extern void         m68k_write_memory_32(unsigned int addr, unsigned int val);

#define M68K_REG_D0  0
#define M68K_REG_D1  1
#define M68K_REG_D2  2
#define M68K_REG_D3  3
#define M68K_REG_D4  4
#define M68K_REG_D5  5
#define M68K_REG_D6  6
#define M68K_REG_D7  7
#define M68K_REG_A0  8
#define M68K_REG_A1  9
#define M68K_REG_A2 10
#define M68K_REG_A3 11
#define M68K_REG_A4 12
#define M68K_REG_A5 13
#define M68K_REG_A6 14
#define M68K_REG_A7 15

/* =========================================================================
 * External helpers for guest memory allocation and font bitmap
 * ========================================================================= */

extern void dos_AllocMem_glue(uint32_t size, uint32_t reqs, uint32_t *out_addr);
extern void dos_FreeMem_glue(uint32_t addr, uint32_t size);
extern const uint8_t g_font8x16[95][16];
extern void Task_Yield(void);

/* AmigaOS memory-type flags (forward definition; full set lives in dos_lib.c). */
#define MEMF_PUBLIC 0x00000001u
#define MEMF_CHIP   0x00000002u
#define MEMF_FAST   0x00000004u

/* Forward display-mode database layout used by Tier 3 copper builder. */
#define AGA_FLAG 0x80000000u

#define DI_MODEID      0
#define DI_WIDTH       4
#define DI_HEIGHT      6
#define DI_DEPTH       8
#define DI_FLAGS       9
#define DI_NAME       10
#define DI_SIZE       14

static uint32_t disp_find_mode(uint32_t mode_id);

/* =========================================================================
 * LVO slot table for graphics.library
 *
 * Slot = |LVO| / 6.  Real AmigaOS graphics.library jump table ranges from
 * LVO -30 (slot 5) to LVO -1056 (slot 176).  Slots 0-4 are unused (LVOs 0-24
 * are not part of the public jump table), and several slots inside the range
 * are reserved by Commodore/AmigaOS.
 * ========================================================================= */

#define GFX_SLOT_MIN   5
#define GFX_SLOT_MAX 176

#define GFX_SLOT_BLITBITMAP              5
#define GFX_SLOT_BLTTEMPLATE             6
#define GFX_SLOT_CLEAREOL                7
#define GFX_SLOT_CLEARSCREEN             8
#define GFX_SLOT_TEXTLENGTH              9
#define GFX_SLOT_TEXT                   10
#define GFX_SLOT_SETFONT                11
#define GFX_SLOT_OPENFONT               12
#define GFX_SLOT_CLOSEFONT              13
#define GFX_SLOT_ASKSOFTSTYLE           14
#define GFX_SLOT_SETSOFTSTYLE           15
#define GFX_SLOT_ADDBOB                 16
#define GFX_SLOT_ADDVSPRITE             17
#define GFX_SLOT_DOCOLLISION            18
#define GFX_SLOT_DRAWGLIST              19
#define GFX_SLOT_INITGELS               20
#define GFX_SLOT_INITMASKS              21
#define GFX_SLOT_REMIBOB                22
#define GFX_SLOT_REMVSPRITE             23
#define GFX_SLOT_SETCOLLISION           24
#define GFX_SLOT_SORTGLIST              25
#define GFX_SLOT_ADDANIMOB              26
#define GFX_SLOT_ANIMATE                27
#define GFX_SLOT_GETGBUFFERS            28
#define GFX_SLOT_INITGMASKS             29
#define GFX_SLOT_DRAWELLIPSE            30
#define GFX_SLOT_AREAELLIPSE            31
#define GFX_SLOT_LOADRGB4               32
#define GFX_SLOT_INITRASTPORT           33
#define GFX_SLOT_INITVPORT              34
#define GFX_SLOT_MRGCOP                 35
#define GFX_SLOT_MAKEVPORT              36
#define GFX_SLOT_LOADVIEW               37
#define GFX_SLOT_WAITBLIT               38
#define GFX_SLOT_SETRAST                39
#define GFX_SLOT_MOVE                   40
#define GFX_SLOT_DRAW                   41
#define GFX_SLOT_AREAMOVE               42
#define GFX_SLOT_AREADRAW               43
#define GFX_SLOT_AREAEND                44
#define GFX_SLOT_WAITTOF                45
#define GFX_SLOT_QBLIT                  46
#define GFX_SLOT_INITAREA               47
#define GFX_SLOT_SETRGB4                48
#define GFX_SLOT_QBSBLIT                49
#define GFX_SLOT_BLTCLEAR               50
#define GFX_SLOT_RECTFILL               51
#define GFX_SLOT_BLTPATTERN             52
#define GFX_SLOT_READPIXEL             53
#define GFX_SLOT_WRITEPIXEL            54
#define GFX_SLOT_FLOOD                 55
#define GFX_SLOT_POLYDRAW              56
#define GFX_SLOT_SETAPEN               57
#define GFX_SLOT_SETBPEN               58
#define GFX_SLOT_SETDRMD               59
#define GFX_SLOT_INITVIEW              60
#define GFX_SLOT_CBUMP                 61
#define GFX_SLOT_CMOVE                 62
#define GFX_SLOT_CWAIT                 63
#define GFX_SLOT_VBEAMPOS              64
#define GFX_SLOT_INITBITMAP            65
#define GFX_SLOT_SCROLLRASTER          66
#define GFX_SLOT_WAITBOVP              67
#define GFX_SLOT_GETSPRITE             68
#define GFX_SLOT_FREESPRITE            69
#define GFX_SLOT_CHANGESPRITE          70
#define GFX_SLOT_MOVESPRITE            71
#define GFX_SLOT_LOCKLAYERROM          72
#define GFX_SLOT_UNLOCKLAYERROM        73
#define GFX_SLOT_SYNCSBITMAP           74
#define GFX_SLOT_COPYSBITMAP           75
#define GFX_SLOT_OWNBLITTER            76
#define GFX_SLOT_DISOWNBLITTER         77
#define GFX_SLOT_INITTMPRAS            78
#define GFX_SLOT_ASKFONT               79
#define GFX_SLOT_ADDFONT               80
#define GFX_SLOT_REMFONT               81
#define GFX_SLOT_ALLOCRASTER           82
#define GFX_SLOT_FREERASTER            83
#define GFX_SLOT_ANDRECTREGION         84
#define GFX_SLOT_ORRECTREGION          85
#define GFX_SLOT_NEWREGION             86
#define GFX_SLOT_CLEARRECTREGION       87
#define GFX_SLOT_CLEARREGION           88
#define GFX_SLOT_DISPOSEREGION         89
#define GFX_SLOT_FREEVPORTCOPSLISTS    90
#define GFX_SLOT_FREECOPLIST           91
#define GFX_SLOT_CLIPBLIT              92
#define GFX_SLOT_XORRECTREGION         93
#define GFX_SLOT_FREECPRLIST           94
#define GFX_SLOT_GETCOLORMAP           95
#define GFX_SLOT_FREECOLORMAP          96
#define GFX_SLOT_GETRGB4               97
#define GFX_SLOT_SCROLLVPORT           98
#define GFX_SLOT_UCOPPERLISTINIT       99
#define GFX_SLOT_FREEGBUFFERS          100
#define GFX_SLOT_BLTBITMAPRASTPORT     101
#define GFX_SLOT_ORREGIONREGION        102
#define GFX_SLOT_XORREGIONREGION       103
#define GFX_SLOT_ANDREGIONREGION       104
#define GFX_SLOT_SETRGB4CM             105
#define GFX_SLOT_BLTMASKBITMAPRASTPORT 106
/* slots 107-108 reserved */
#define GFX_SLOT_ATTEMPTLOCKLAYERROM   109
#define GFX_SLOT_GFXNEW                110
#define GFX_SLOT_GFXFREE               111
#define GFX_SLOT_GFXASSOCIATE          112
#define GFX_SLOT_BITMAPSCALE           113
#define GFX_SLOT_SCALERDIV             114
#define GFX_SLOT_TEXTEXTENT            115
#define GFX_SLOT_TEXTFIT               116
#define GFX_SLOT_GFXLOOKUP             117
#define GFX_SLOT_VIDEOCONTROL          118
#define GFX_SLOT_OPENMONITOR           119
#define GFX_SLOT_CLOSEMONITOR          120
#define GFX_SLOT_FINDDISPLAYINFO       121
#define GFX_SLOT_NEXTDISPLAYINFO       122
/* slots 123-125 reserved */
#define GFX_SLOT_GETDISPLAYINFODATA    126
#define GFX_SLOT_FONTEXTENT            127
#define GFX_SLOT_READPIXELLINE8        128
#define GFX_SLOT_WRITEPIXELLINE8       129
#define GFX_SLOT_READPIXELARRAY8       130
#define GFX_SLOT_WRITEPIXELARRAY8      131
#define GFX_SLOT_GETVPMODEID           132
#define GFX_SLOT_MODENOTAVAILABLE      133
#define GFX_SLOT_WEIGHTAMATCH          134
#define GFX_SLOT_ERASERECT             135
#define GFX_SLOT_EXTENDFONT            136
#define GFX_SLOT_STRIPFONT             137
#define GFX_SLOT_CALCIVG               138
#define GFX_SLOT_ATTACHPALEXTRA        139
#define GFX_SLOT_OBTAINBESTPENA        140
#define GFX_SLOT_SETRGB32              142
#define GFX_SLOT_GETAPEN               143
#define GFX_SLOT_GETBPEN               144
#define GFX_SLOT_GETDRMD               145
#define GFX_SLOT_GETOUTLINEPEN         146
#define GFX_SLOT_LOADRGB32             147
#define GFX_SLOT_SETCHIPREV            148
#define GFX_SLOT_SETABPENDRMD          149
#define GFX_SLOT_GETRGB32              150
#define GFX_SLOT_ALLOCBITMAP           153
#define GFX_SLOT_FREEBITMAP            154
#define GFX_SLOT_GETEXTSPRITEA         155
#define GFX_SLOT_COERCEMODE            156
#define GFX_SLOT_CHANGEVPBITMAP        157
#define GFX_SLOT_RELEASEPEN            158
#define GFX_SLOT_OBTAINPEN             159
#define GFX_SLOT_GETBITMAPATTR         160
#define GFX_SLOT_ALLOCDBUFINFO         161
#define GFX_SLOT_FREEDBUFINFO          162
#define GFX_SLOT_SETOUTLINEPEN         163
#define GFX_SLOT_SETWRITEMASK          164
#define GFX_SLOT_SETMAXPEN             165
#define GFX_SLOT_SETRGB32CM            166
#define GFX_SLOT_SCROLLRASTERBF        167
#define GFX_SLOT_FINDCOLOR             168
#define GFX_SLOT_ALLOCSPRITEDATAA      170
#define GFX_SLOT_CHANGEEXTSPRITEA      171
#define GFX_SLOT_FREESPRITEDATA        172
#define GFX_SLOT_SETRPATTRSA           173
#define GFX_SLOT_GETRPATTRSA           174
#define GFX_SLOT_BESTMODEIDA           175
#define GFX_SLOT_WRITECHUNKYPIXELS     176

/* =========================================================================
 * Helper: read / write RastPort fields in guest RAM
 * ========================================================================= */

static inline uint8_t  rp_u8 (uint32_t rp, int off)
    { return (uint8_t)m68k_read_memory_8(rp + off); }
static inline uint16_t rp_u16(uint32_t rp, int off)
    { return (uint16_t)m68k_read_memory_16(rp + off); }
static inline int16_t  rp_s16(uint32_t rp, int off)
    { return (int16_t)m68k_read_memory_16(rp + off); }
static inline void rp_w_u8 (uint32_t rp, int off, uint8_t  v)
    { m68k_write_memory_8(rp + off, v); }
static inline void rp_w_u16(uint32_t rp, int off, uint16_t v)
    { m68k_write_memory_16(rp + off, v); }
static inline void rp_w_s16(uint32_t rp, int off, int16_t  v)
    { m68k_write_memory_16(rp + off, (uint16_t)v); }

/* =========================================================================
 * Helpers: pen -> RGB, line drawing
 * ========================================================================= */

static uint8_t rp_pen_clamped(uint32_t rp, int off)
{
    if (!rp) return 0;
    uint8_t pen = rp_u8(rp, off);
    uint8_t max = rp_u8(rp, RP_OFF_MAXPEN);
    if (max > 0 && pen > max) pen = max;
    return pen;
}

static uint32_t current_fg(uint32_t rp)
{
    if (!rp) return 1;
    return rp_pen_clamped(rp, RP_OFF_FGPEN);
}

static uint32_t current_bg(uint32_t rp)
{
    if (!rp) return 0;
    return rp_pen_clamped(rp, RP_OFF_BGPEN);
}

static uint32_t current_outline(uint32_t rp)
{
    if (!rp) return 0;
    return rp_pen_clamped(rp, RP_OFF_OLNPEN);
}

static int current_mode(uint32_t rp)
{
    if (!rp) return JAM1;
    return (int)rp_u8(rp, RP_OFF_DRAWMODE);
}

/* Helper: 32-bit 0x00RRGGBB → 8-bit greyscale */
static uint8_t rgb_to_grey8(uint32_t rgb)
{
    uint8_t r = (uint8_t)((rgb >> 16) & 0xFF);
    uint8_t g = (uint8_t)((rgb >> 8) & 0xFF);
    uint8_t b = (uint8_t)(rgb & 0xFF);
    return (uint8_t)((r + g + b) / 3);
}

/* =========================================================================
 * Font / text metric helpers
 * ========================================================================= */

typedef struct {
    const char *name;
    uint16_t    ySize;
    uint16_t    xSize;
    uint16_t    baseline;
    uint8_t     style;
    uint8_t     flags;
    uint32_t    font_addr;   /* guest TextFont instance */
    uint32_t    bitmap_addr; /* guest char bitmap */
    uint32_t    ref_count;
} UaosFontEntry;

static UaosFontEntry font_entries[] = {
    {"topaz.font",     8,  8, 16, 12, 0, 0, 0, 0},
    {"topaz.font",    11,  8, 11,  8, 0, 0, 0, 0},
    {"topaz.font",    13,  8, 13, 10, 0, 0, 0, 0},
    {"emerald.font",   8,  8, 16, 12, 0, 0, 0, 0},
    {"sapphire.font",  8,  8, 16, 12, 0, 0, 0, 0},
    {"ruby.font",      8,  8, 16, 12, 0, 0, 0, 0},
    {"garnet.font",    8,  8, 16, 12, 0, 0, 0, 0},
    {"crystal.font",    11,  8, 11,  8, 0, 0, 0, 0},
    {"diamond.font",    11,  8, 11,  8, 0, 0, 0, 0},
    {"platinum.font",   8,  8, 16, 12, 0, 0, 0, 0},
    {"tiffany.font",    8,  8, 16, 12, 0, 0, 0, 0},
};
#define UAOS_FONT_COUNT (sizeof(font_entries) / sizeof(font_entries[0]))

static int match_font_name(const char *name, const char *expected)
{
    for (int i = 0; ; i++) {
        char a = name[i];
        char b = expected[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
        if (a == 0) return 1;
    }
}

static uint32_t create_font_bitmap(UaosFontEntry *e)
{
    if (e->bitmap_addr) return e->bitmap_addr;

    uint32_t bitmap_addr = 0;
    dos_AllocMem_glue(95 * 16, 0, &bitmap_addr);
    if (!bitmap_addr) return 0;

    for (int i = 0; i < 95 * 16; i++) {
        int ch = i / 16;
        int row = i % 16;
        m68k_write_memory_8(bitmap_addr + i, g_font8x16[ch][row]);
    }
    e->bitmap_addr = bitmap_addr;
    return bitmap_addr;
}

static uint32_t create_font_instance(UaosFontEntry *e)
{
    if (e->font_addr) return e->font_addr;

    uint32_t bitmap = create_font_bitmap(e);
    if (!bitmap) return 0;

    uint32_t font_addr = 0;
    dos_AllocMem_glue(TF_SIZE, 0, &font_addr);
    if (!font_addr) return 0;

    for (int i = 0; i < TF_SIZE; i++)
        m68k_write_memory_8(font_addr + i, 0);

    m68k_write_memory_16(font_addr + TF_OFF_YSIZE,     e->ySize);
    m68k_write_memory_8 (font_addr + TF_OFF_STYLE,     e->style);
    m68k_write_memory_8 (font_addr + TF_OFF_FLAGS,     e->flags);
    m68k_write_memory_16(font_addr + TF_OFF_XSIZE,     e->xSize);
    m68k_write_memory_16(font_addr + TF_OFF_BASELINE,  e->baseline);
    m68k_write_memory_16(font_addr + TF_OFF_BOLDSMEAR, 1);
    m68k_write_memory_16(font_addr + TF_OFF_ACCESSORS, 1);
    m68k_write_memory_8 (font_addr + TF_OFF_LOCHAR,  0x20);
    m68k_write_memory_8 (font_addr + TF_OFF_HICHAR,  0x7E);
    m68k_write_memory_32(font_addr + TF_OFF_CHARDATA, bitmap);
    m68k_write_memory_16(font_addr + TF_OFF_MODULO,    1);
    m68k_write_memory_32(font_addr + TF_OFF_CHARSPACE, 0);
    m68k_write_memory_32(font_addr + TF_OFF_CHARKERN, 0);

    e->font_addr = font_addr;
    return font_addr;
}

static UaosFontEntry *font_find_entry(const char *name, uint16_t ySize,
                                      uint8_t style, uint8_t flags)
{
    (void)style; (void)flags;
    if (!name || name[0] == 0) return NULL;

    UaosFontEntry *best = NULL;
    int best_diff = 0x7FFFFFFF;

    for (size_t i = 0; i < UAOS_FONT_COUNT; i++) {
        if (!match_font_name(name, font_entries[i].name)) continue;
        int target = (ySize == 0) ? (int)font_entries[i].ySize : (int)ySize;
        int diff = (int)font_entries[i].ySize - target;
        if (diff < 0) diff = -diff;
        if (diff < best_diff) {
            best_diff = diff;
            best = &font_entries[i];
        }
    }
    return best;
}

static uint32_t font_open(const char *name, uint16_t ySize,
                          uint8_t style, uint8_t flags)
{
    UaosFontEntry *e = font_find_entry(name, ySize, style, flags);
    if (!e) return 0;
    uint32_t font = create_font_instance(e);
    if (font) e->ref_count++;
    return font;
}

static int font_is_known(uint32_t font)
{
    for (size_t i = 0; i < UAOS_FONT_COUNT; i++)
        if (font_entries[i].font_addr == font) return 1;
    return 0;
}

static UaosFontEntry *font_entry_by_addr(uint32_t font)
{
    for (size_t i = 0; i < UAOS_FONT_COUNT; i++)
        if (font_entries[i].font_addr == font) return &font_entries[i];
    return NULL;
}

static void font_close(uint32_t font)
{
    UaosFontEntry *e = font_entry_by_addr(font);
    if (!e || e->ref_count == 0) return;
    e->ref_count--;
}

static uint32_t create_builtin_font(void)
{
    return font_open("topaz.font", 8, 0, 0);
}

static int text_char_width(uint32_t rp)
{
    if (!rp) return FB_CharWidth();
    uint32_t font = m68k_read_memory_32(rp + RP_OFF_FONT);
    if (font) return (int)m68k_read_memory_16(font + TF_OFF_XSIZE);
    return FB_CharWidth();
}

static int text_char_height(uint32_t rp)
{
    if (!rp) return FB_CharHeight();
    uint32_t font = m68k_read_memory_32(rp + RP_OFF_FONT);
    if (font) return (int)m68k_read_memory_16(font + TF_OFF_YSIZE);
    return FB_CharHeight();
}

/* =========================================================================
 * Blitting surface helpers
 * ========================================================================= */

typedef struct {
    uint32_t  planes[8];
    uint16_t  bpr;
    uint16_t  width;
    uint16_t  height;
    uint8_t   depth;   /* 0 = framebuffer (chunky), 1-8 = planar BitMap */
    uint8_t   is_fb;
    uint32_t  rp;
    int       dx, dy;  /* UAOS: window offset applied to framebuffer coords */
} BlitSurface;

static uint32_t rp_pen_to_rgb(uint32_t rp, uint32_t pen)
{
    (void)rp;
    return amiga_pen_to_rgb((uint8_t)pen);
}

static uint32_t rp_rgb_to_pen(uint32_t rp, uint32_t rgb)
{
    (void)rp;
    /* Default inverse of the Amiga 4-bit palette (entries 0..15). */
    uint8_t r = (uint8_t)((rgb >> 16) & 0xFF);
    uint8_t g = (uint8_t)((rgb >> 8) & 0xFF);
    uint8_t b = (uint8_t)(rgb & 0xFF);
    uint32_t best = 0;
    uint32_t best_dist = 0xFFFFFFFF;
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t c = amiga_pen_to_rgb((uint8_t)i);
        int dr = (int)((c >> 16) & 0xFF) - (int)r;
        int dg = (int)((c >> 8) & 0xFF) - (int)g;
        int db = (int)(c & 0xFF) - (int)b;
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < best_dist) {
            best_dist = d;
            best = i;
        }
    }
    return best;
}

static uint32_t apply_write_mask(uint32_t rp, uint32_t dst, uint32_t src)
{
    if (!rp) return src;
    uint8_t mask = rp_u8(rp, RP_OFF_MASK);
    if (mask == 0xFF) return src;
    if (mask == 0) return dst;
    uint32_t mask32 = mask | (mask << 8) | (mask << 16) | (mask << 24);
    return (dst & ~mask32) | (src & mask32);
}

static uint32_t planar_pixel_get(BlitSurface *s, int x, int y)
{
    uint32_t pen = 0;
    for (int p = 0; p < s->depth; p++) {
        uint32_t base = s->planes[p];
        if (!base) continue;
        uint32_t byte = base + (uint32_t)y * s->bpr + (uint32_t)(x / 8);
        uint8_t b = m68k_read_memory_8(byte);
        if (b & (1 << (7 - (x & 7)))) pen |= (1u << p);
    }
    return pen;
}

static void planar_pixel_put(BlitSurface *s, int x, int y, uint32_t pen)
{
    for (int p = 0; p < s->depth; p++) {
        uint32_t base = s->planes[p];
        if (!base) continue;
        uint32_t byte = base + (uint32_t)y * s->bpr + (uint32_t)(x / 8);
        uint8_t b = m68k_read_memory_8(byte);
        uint8_t bit = (uint8_t)(1 << (7 - (x & 7)));
        if (pen & (1u << p)) b |= bit;
        else b &= ~bit;
        m68k_write_memory_8(byte, b);
    }
}

static void blit_surface_from_bitmap(BlitSurface *s, uint32_t bm)
{
    s->dx = 0;
    s->dy = 0;
    if (!bm) {
        s->is_fb = 1;
        s->bpr = 0;
        s->width = (uint16_t)g_fb.width;
        s->height = (uint16_t)g_fb.height;
        s->depth = 0;
        s->rp = 0;
        for (int i = 0; i < 8; i++) s->planes[i] = 0;
        return;
    }
    s->is_fb = 0;
    s->bpr = m68k_read_memory_16(bm + BM_OFF_BYTESPERROW);
    s->width = (uint16_t)(s->bpr * 8);
    s->height = m68k_read_memory_16(bm + BM_OFF_ROWS);
    s->depth = m68k_read_memory_8(bm + BM_OFF_DEPTH);
    if (s->depth == 0) s->depth = 1;
    if (s->depth > 8) s->depth = 8;
    s->rp = 0;
    for (int i = 0; i < 8; i++)
        s->planes[i] = m68k_read_memory_32(bm + BM_OFF_PLANES + i * 4);
}

static void blit_surface_from_rastport(BlitSurface *s, uint32_t rp)
{
    s->rp = rp;
    s->dx = 0;
    s->dy = 0;
    if (!rp) {
        s->is_fb = 1;
        s->bpr = 0;
        s->width = (uint16_t)g_fb.width;
        s->height = (uint16_t)g_fb.height;
        s->depth = 0;
        for (int i = 0; i < 8; i++) s->planes[i] = 0;
        return;
    }
    uint32_t bm = m68k_read_memory_32(rp + RP_OFF_BITMAP);
    if (!bm) {
        s->is_fb = 1;
        s->bpr = 0;
        s->width = (uint16_t)g_fb.width;
        s->height = (uint16_t)g_fb.height;
        s->depth = 0;
        for (int i = 0; i < 8; i++) s->planes[i] = 0;

        /* If the RastPort's Layer field points to a UAOS Window, add the
         * window's screen position (plus GimmeZeroZero border offset) so that
         * guest drawing operations are relative to the window content area. */
        uint32_t win = m68k_read_memory_32(rp + RP_OFF_LAYER);
        if (win) {
            int wx = (int)(int16_t)m68k_read_memory_16(win + WIN_OFF_LEFTEDGE);
            int wy = (int)(int16_t)m68k_read_memory_16(win + WIN_OFF_TOPEDGE);
            uint32_t flags = m68k_read_memory_32(win + WIN_OFF_FLAGS);
            int bx = 0, by = 0;
            if (flags & WFLG_GIMMEZEROZERO) {
                bx = WM_BORDER;
                by = WM_TITLEBAR_H;
            }
            s->dx = wx + bx;
            s->dy = wy + by;
        }
    } else {
        blit_surface_from_bitmap(s, bm);
        s->rp = rp;
    }
}

static int blit_surface_get(BlitSurface *s, int x, int y, uint32_t *pixel)
{
    x += s->dx;
    y += s->dy;
    if (s->is_fb) {
        if (x < 0 || x >= (int)g_fb.width || y < 0 || y >= (int)g_fb.height) return 0;
        *pixel = FB_GetPixel(x, y);
        return 1;
    }
    if (x < 0 || y < 0 || x >= s->width || y >= s->height) return 0;
    *pixel = planar_pixel_get(s, x, y);
    return 1;
}

static int blit_surface_put(BlitSurface *s, int x, int y, uint32_t pixel)
{
    x += s->dx;
    y += s->dy;
    if (s->is_fb) {
        if (x < 0 || x >= (int)g_fb.width || y < 0 || y >= (int)g_fb.height) return 0;
        uint32_t dst = FB_GetPixel(x, y);
        uint32_t src = rp_pen_to_rgb(s->rp, pixel);
        FB_PutPixel(x, y, apply_write_mask(s->rp, dst, src));
        return 1;
    }
    if (x < 0 || y < 0 || x >= s->width || y >= s->height) return 0;
    uint32_t dst = planar_pixel_get(s, x, y);
    uint32_t out = apply_write_mask(s->rp, dst, pixel);
    planar_pixel_put(s, x, y, out);
    return 1;
}

static int planar_mask_get(uint32_t mask_base, int mask_bpr, int x, int y)
{
    if (!mask_base || mask_bpr <= 0) return 1;
    int byte = y * mask_bpr + (x / 8);
    int bit = 7 - (x % 8);
    uint8_t b = m68k_read_memory_8(mask_base + byte);
    return (b & (1 << bit)) ? 1 : 0;
}

static uint32_t apply_minterm(uint32_t src, uint32_t dst, uint8_t minterm)
{
    switch (minterm) {
        case 0x00: return 0;
        case 0xFF: return 0xFFFFFFFF;
        case 0xC0: return src;           /* copy source */
        case 0x30: return ~src;           /* inverse source */
        case 0x0C: return dst;           /* keep dest */
        case 0xF0: return ~dst;           /* inverse dest */
        case 0x3C: return src ^ dst;     /* XOR */
        case 0xFC: return src | dst;     /* OR */
        case 0x0F: return ~(src | dst);    /* NOR */
        case 0xCC: return src & dst;     /* AND */
        default:   return src;           /* default copy */
    }
}

static uint32_t blit_color_convert(uint32_t rp, uint32_t val, int from_fb, int to_fb)
{
    if (from_fb == to_fb) return val;
    if (from_fb && !to_fb) return rp_rgb_to_pen(rp, val); /* RGB -> pen */
    return rp_pen_to_rgb(rp, val);                       /* pen -> RGB */
}

static void blit_rect(BlitSurface *src, int sx, int sy,
                      BlitSurface *dst, int dx, int dy,
                      int w, int h, uint8_t minterm,
                      uint32_t mask_base, int mask_bpr, int mask_x, int mask_y)
{
    if (w <= 0 || h <= 0) return;

    /* Allocate a temporary copy of the source rectangle so overlapping
     * source and destination are handled safely. */
    uint32_t tmp_size = (uint32_t)w * (uint32_t)h * 4;
    uint32_t tmp = 0;
    dos_AllocMem_glue(tmp_size, 0, &tmp);
    if (!tmp) return;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = 0;
            if (!blit_surface_get(src, sx + x, sy + y, &p)) p = 0;
            m68k_write_memory_32(tmp + ((uint32_t)y * (uint32_t)w + (uint32_t)x) * 4, p);
        }
    }

    uint32_t rp = dst->rp ? dst->rp : src->rp;
    int src_fb = src->is_fb;
    int dst_fb = dst->is_fb;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (!planar_mask_get(mask_base, mask_bpr, mask_x + x, mask_y + y)) continue;

            uint32_t src_pixel = m68k_read_memory_32(tmp + ((uint32_t)y * (uint32_t)w + (uint32_t)x) * 4);
            uint32_t dst_pixel = 0;
            blit_surface_get(dst, dx + x, dy + y, &dst_pixel);

            /* Normalise to the destination colour space before applying the
             * minterm so bitwise operations are meaningful. */
            uint32_t s = blit_color_convert(rp, src_pixel, src_fb, dst_fb);
            uint32_t out = apply_minterm(s, dst_pixel, minterm);
            blit_surface_put(dst, dx + x, dy + y, out);
        }
    }

    dos_FreeMem_glue(tmp, tmp_size);
}

static void rp_put_pixel(uint32_t rp, int x, int y, uint32_t pen)
{
    BlitSurface s;
    blit_surface_from_rastport(&s, rp);
    blit_surface_put(&s, x, y, pen);
}

static void planar_fill_rect(BlitSurface *s, int x, int y, int w, int h, uint32_t pen)
{
    if (w <= 0 || h <= 0) return;
    int x1 = x;
    int y1 = y;
    int x2 = x + w - 1;
    int y2 = y + h - 1;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= s->width) x2 = s->width - 1;
    if (y2 >= s->height) y2 = s->height - 1;
    if (x1 > x2 || y1 > y2) return;

    int start_byte = x1 / 8;
    int end_byte = x2 / 8;
    int start_bit = 7 - (x1 & 7);
    int end_bit = 7 - (x2 & 7);

    for (int p = 0; p < s->depth; p++) {
        uint32_t base = s->planes[p];
        if (!base) continue;
        uint8_t bit_val = (uint8_t)((pen >> p) & 1);
        for (int yy = y1; yy <= y2; yy++) {
            uint32_t row = base + (uint32_t)yy * s->bpr;
            for (int b = start_byte; b <= end_byte; b++) {
                uint8_t mask;
                if (b == start_byte && b == end_byte) {
                    mask = (uint8_t)(((1 << (start_bit + 1)) - 1) & ~((1 << end_bit) - 1));
                } else if (b == start_byte) {
                    mask = (uint8_t)((1 << (start_bit + 1)) - 1);
                } else if (b == end_byte) {
                    mask = (uint8_t)(~((1 << end_bit) - 1));
                } else {
                    mask = 0xFF;
                }
                uint8_t old = m68k_read_memory_8(row + b);
                uint8_t new_val;
                if (bit_val) new_val = old | mask;
                else new_val = old & ~mask;
                m68k_write_memory_8(row + b, new_val);
            }
        }
    }
}

static void rp_fill_rect(uint32_t rp, int x, int y, int w, int h, uint32_t pen)
{
    if (w <= 0 || h <= 0) return;
    BlitSurface s;
    blit_surface_from_rastport(&s, rp);
    if (s.is_fb) {
        FB_FillRect(x + s.dx, y + s.dy, w, h, rp_pen_to_rgb(rp, pen));
        return;
    }
    planar_fill_rect(&s, x, y, w, h, pen);
}

static int text_baseline(uint32_t rp)
{
    uint32_t font = rp ? m68k_read_memory_32(rp + RP_OFF_FONT) : 0;
    if (font) return (int)m68k_read_memory_16(font + TF_OFF_BASELINE);
    return 12;
}

static int text_width(uint32_t rp, uint32_t str, int len)
{
    (void)str;
    if (len < 0) len = 0;
    return len * text_char_width(rp);
}

static int text_height(uint32_t rp)
{
    return text_char_height(rp);
}

static void fill_text_extent(uint32_t te, int x, int y, int w, int h, int baseline)
{
    if (!te) return;
    for (int i = 0; i < TE_SIZE; i++)
        m68k_write_memory_8(te + i, 0);
    m68k_write_memory_16(te + TE_OFF_EXTENT_X,      (int16_t)x);
    m68k_write_memory_16(te + TE_OFF_EXTENT_Y,      (int16_t)y);
    m68k_write_memory_16(te + TE_OFF_EXTENT_WIDTH,  (int16_t)w);
    m68k_write_memory_16(te + TE_OFF_EXTENT_HEIGHT, (int16_t)h);
    m68k_write_memory_16(te + TE_OFF_WIDTH,         (int16_t)w);
    m68k_write_memory_16(te + TE_OFF_HEIGHT,        (int16_t)h);
    (void)baseline;
}

/* Bresenham line — clipped per-pixel by the RastPort surface */
static void draw_line(uint32_t rp, int x0, int y0, int x1, int y1, uint32_t pen)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int sx = dx > 0 ? 1 : -1;
    int sy = dy > 0 ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    if (dx >= dy) {
        int err = dx / 2;
        for (int x = x0, y = y0, i = 0; i <= dx; i++, x += sx) {
            rp_put_pixel(rp, x, y, pen);
            err -= dy;
            if (err < 0) { y += sy; err += dx; }
        }
    } else {
        int err = dy / 2;
        for (int x = x0, y = y0, i = 0; i <= dy; i++, y += sy) {
            rp_put_pixel(rp, x, y, pen);
            err -= dx;
            if (err < 0) { x += sx; err += dy; }
        }
    }
}

/* =========================================================================
 * Ellipse drawing helper
 * ========================================================================= */

static void plot_ellipse_points(uint32_t rp, int cx, int cy, int x, int y, uint32_t pen, int fill)
{
    if (fill) {
        if (x >= 0) {
            draw_line(rp, cx - x, cy + y, cx + x, cy + y, pen);
            draw_line(rp, cx - x, cy - y, cx + x, cy - y, pen);
        }
    } else {
        rp_put_pixel(rp, cx + x, cy + y, pen);
        rp_put_pixel(rp, cx - x, cy + y, pen);
        rp_put_pixel(rp, cx + x, cy - y, pen);
        rp_put_pixel(rp, cx - x, cy - y, pen);
    }
}

static void draw_ellipse(uint32_t rp, int cx, int cy, int rx, int ry, uint32_t pen, int fill)
{
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx == 0 && ry == 0) { rp_put_pixel(rp, cx, cy, pen); return; }
    if (rx == 0) {
        for (int y = -ry; y <= ry; y++) rp_put_pixel(rp, cx, cy + y, pen);
        return;
    }
    if (ry == 0) {
        for (int x = -rx; x <= rx; x++) rp_put_pixel(rp, cx + x, cy, pen);
        return;
    }

    long long rx2 = (long long)rx * rx;
    long long ry2 = (long long)ry * ry;
    long long two_rx2 = 2 * rx2;
    long long two_ry2 = 2 * ry2;

    int x = 0, y = ry;
    long long dx = two_ry2 * x;
    long long dy = two_rx2 * y;
    long long p1 = ry2 - rx2 * ry + rx2 / 4;

    while (dx < dy) {
        plot_ellipse_points(rp, cx, cy, x, y, pen, fill);
        x++;
        dx += two_ry2;
        if (p1 < 0) {
            p1 += ry2 + dx;
        } else {
            y--;
            dy -= two_rx2;
            p1 += ry2 + dx - dy;
        }
    }

    long long p2 = ry2 * ((long long)x * x + x) + rx2 * ((long long)y * y - y) - rx2 * ry2;
    while (y >= 0) {
        plot_ellipse_points(rp, cx, cy, x, y, pen, fill);
        y--;
        dy -= two_rx2;
        if (p2 > 0) {
            p2 += rx2 - dy;
        } else {
            x++;
            dx += two_ry2;
            p2 += rx2 - dy + dx;
        }
    }
}

/* =========================================================================
 * Polygon state for AreaMove / AreaDraw / AreaEnd
 * ========================================================================= */

#define MAX_POLY_POINTS 64
#define MAX_POLYGONS    16

typedef struct {
    uint32_t rp_key;
    int count;
    int points[MAX_POLY_POINTS][2];
} PolygonState;

static PolygonState g_polygons[MAX_POLYGONS];

static PolygonState *poly_get(uint32_t rp)
{
    if (!rp) return NULL;
    for (int i = 0; i < MAX_POLYGONS; i++)
        if (g_polygons[i].rp_key == rp) return &g_polygons[i];
    for (int i = 0; i < MAX_POLYGONS; i++) {
        if (g_polygons[i].rp_key == 0) {
            g_polygons[i].rp_key = rp;
            g_polygons[i].count = 0;
            return &g_polygons[i];
        }
    }
    return NULL;
}

static void poly_reset(uint32_t rp)
{
    PolygonState *p = poly_get(rp);
    if (p) p->count = 0;
}

static void fill_polygon(uint32_t rp, PolygonState *p, uint32_t pen)
{
    if (!p || p->count < 3) return;

    int min_y = p->points[0][1], max_y = p->points[0][1];
    for (int i = 1; i < p->count; i++) {
        if (p->points[i][1] < min_y) min_y = p->points[i][1];
        if (p->points[i][1] > max_y) max_y = p->points[i][1];
    }

    int xs[MAX_POLY_POINTS];
    for (int y = min_y; y <= max_y; y++) {
        int n = 0;
        for (int i = 0; i < p->count; i++) {
            int j = (i + 1) % p->count;
            int y1 = p->points[i][1];
            int y2 = p->points[j][1];
            if ((y1 <= y && y < y2) || (y2 <= y && y < y1)) {
                int x1 = p->points[i][0];
                int x2 = p->points[j][0];
                int x = x1 + (int)(((long long)(y - y1) * (x2 - x1)) / (y2 - y1));
                if (n < MAX_POLY_POINTS) xs[n++] = x;
            }
        }
        for (int i = 0; i < n - 1; i++)
            for (int j = i + 1; j < n; j++)
                if (xs[i] > xs[j]) { int t = xs[i]; xs[i] = xs[j]; xs[j] = t; }
        for (int i = 0; i < n; i += 2)
            if (i + 1 < n)
                draw_line(rp, xs[i], y, xs[i + 1], y, pen);
    }
}

/* =========================================================================
 * Flood-fill helper
 * ========================================================================= */

static void flood_fill(uint32_t rp, int sx, int sy, uint32_t target_pen, uint32_t fill_pen)
{
    BlitSurface s;
    blit_surface_from_rastport(&s, rp);

    if (s.is_fb) {
        if (sx < 0 || sx >= (int)g_fb.width || sy < 0 || sy >= (int)g_fb.height) return;
    } else {
        if (sx < 0 || sx >= s.width || sy < 0 || sy >= s.height) return;
    }

    uint32_t cur = 0;
    if (!blit_surface_get(&s, sx, sy, &cur) || cur != target_pen) return;

    int stack_x[1024], stack_y[1024];
    int sp = 0;
    stack_x[sp] = sx; stack_y[sp] = sy; sp++;

    while (sp > 0) {
        sp--;
        int x = stack_x[sp], y = stack_y[sp];
        if (s.is_fb && (x < 0 || x >= (int)g_fb.width || y < 0 || y >= (int)g_fb.height)) continue;
        if (!s.is_fb && (x < 0 || x >= s.width || y < 0 || y >= s.height)) continue;
        if (!blit_surface_get(&s, x, y, &cur) || cur != target_pen) continue;

        int left = x;
        while (left > 0 && blit_surface_get(&s, left - 1, y, &cur) && cur == target_pen) left--;
        int right = x;
        int max_x = s.is_fb ? (int)g_fb.width : s.width;
        while (right < max_x - 1 && blit_surface_get(&s, right + 1, y, &cur) && cur == target_pen) right++;

        for (int i = left; i <= right; i++) blit_surface_put(&s, i, y, fill_pen);

        for (int i = left; i <= right; i++) {
            if (y > 0 && blit_surface_get(&s, i, y - 1, &cur) && cur == target_pen) {
                if (sp < 1024) { stack_x[sp] = i; stack_y[sp] = y - 1; sp++; }
            }
            int max_y = s.is_fb ? (int)g_fb.height : s.height;
            if (y < max_y - 1 && blit_surface_get(&s, i, y + 1, &cur) && cur == target_pen) {
                if (sp < 1024) { stack_x[sp] = i; stack_y[sp] = y + 1; sp++; }
            }
        }
    }
}

/* =========================================================================
 * Region helpers (single bounding-box region representation)
 * ========================================================================= */

static void rg_set_bounds(uint32_t rg, int16_t minx, int16_t miny,
                          int16_t maxx, int16_t maxy)
{
    if (!rg) return;
    m68k_write_memory_16(rg + RG_OFF_MINX, (uint16_t)minx);
    m68k_write_memory_16(rg + RG_OFF_MINY, (uint16_t)miny);
    m68k_write_memory_16(rg + RG_OFF_MAXX, (uint16_t)maxx);
    m68k_write_memory_16(rg + RG_OFF_MAXY, (uint16_t)maxy);

    uint32_t rr = m68k_read_memory_32(rg + RG_OFF_REGIONRECT);
    if (rr) {
        m68k_write_memory_16(rr + RR_OFF_MINX, (uint16_t)minx);
        m68k_write_memory_16(rr + RR_OFF_MINY, (uint16_t)miny);
        m68k_write_memory_16(rr + RR_OFF_MAXX, (uint16_t)maxx);
        m68k_write_memory_16(rr + RR_OFF_MAXY, (uint16_t)maxy);
    }
}

static int rg_is_empty(uint32_t rg)
{
    if (!rg) return 1;
    int16_t minx = (int16_t)m68k_read_memory_16(rg + RG_OFF_MINX);
    int16_t miny = (int16_t)m68k_read_memory_16(rg + RG_OFF_MINY);
    int16_t maxx = (int16_t)m68k_read_memory_16(rg + RG_OFF_MAXX);
    int16_t maxy = (int16_t)m68k_read_memory_16(rg + RG_OFF_MAXY);
    return (minx > maxx || miny > maxy);
}

static void rg_read_bounds(uint32_t rg, int16_t *minx, int16_t *miny,
                           int16_t *maxx, int16_t *maxy)
{
    if (!rg) {
        *minx = 0; *miny = 0; *maxx = -1; *maxy = -1;
        return;
    }
    *minx = (int16_t)m68k_read_memory_16(rg + RG_OFF_MINX);
    *miny = (int16_t)m68k_read_memory_16(rg + RG_OFF_MINY);
    *maxx = (int16_t)m68k_read_memory_16(rg + RG_OFF_MAXX);
    *maxy = (int16_t)m68k_read_memory_16(rg + RG_OFF_MAXY);
}

static void rg_read_rect(uint32_t rect, int16_t *minx, int16_t *miny,
                         int16_t *maxx, int16_t *maxy)
{
    if (!rect) {
        *minx = 0; *miny = 0; *maxx = -1; *maxy = -1;
        return;
    }
    *minx = (int16_t)m68k_read_memory_16(rect + RECT_MINX);
    *miny = (int16_t)m68k_read_memory_16(rect + RECT_MINY);
    *maxx = (int16_t)m68k_read_memory_16(rect + RECT_MAXX);
    *maxy = (int16_t)m68k_read_memory_16(rect + RECT_MAXY);
}

static void rg_union_rect(int16_t ax, int16_t ay, int16_t aw, int16_t ah,
                          int16_t bx, int16_t by, int16_t bw, int16_t bh,
                          int16_t *rx, int16_t *ry, int16_t *rw, int16_t *rh)
{
    if (ax > aw && bx > bw) { /* both empty */
        *rx = 0; *ry = 0; *rw = -1; *rh = -1;
        return;
    }
    if (ax > aw) { *rx = bx; *ry = by; *rw = bw; *rh = bh; return; }
    if (bx > bw) { *rx = ax; *ry = ay; *rw = aw; *rh = ah; return; }
    *rx = (ax < bx) ? ax : bx;
    *ry = (ay < by) ? ay : by;
    *rw = (aw > bw) ? aw : bw;
    *rh = (ah > bh) ? ah : bh;
}

static void rg_intersect_rect(int16_t ax, int16_t ay, int16_t aw, int16_t ah,
                              int16_t bx, int16_t by, int16_t bw, int16_t bh,
                              int16_t *rx, int16_t *ry, int16_t *rw, int16_t *rh)
{
    if (ax > aw || bx > bw) { /* one empty */
        *rx = 0; *ry = 0; *rw = -1; *rh = -1;
        return;
    }
    *rx = (ax > bx) ? ax : bx;
    *ry = (ay > by) ? ay : by;
    *rw = (aw < bw) ? aw : bw;
    *rh = (ah < bh) ? ah : bh;
    if (*rx > *rw || *ry > *rh) {
        *rx = 0; *ry = 0; *rw = -1; *rh = -1;
    }
}

/* =========================================================================
 * ColorMap / palette helpers
 * ========================================================================= */

static uint32_t cm_get_colortable(uint32_t cm)
{
    if (!cm) return 0;
    return m68k_read_memory_32(cm + CM_OFF_COLORTABLE);
}

static uint32_t cm_get_count(uint32_t cm)
{
    if (!cm) return 0;
    return (uint32_t)m68k_read_memory_16(cm + CM_OFF_COUNT);
}

static uint32_t vp_get_colormap(uint32_t vp)
{
    if (!vp) return 0;
    return m68k_read_memory_32(vp + VP_OFF_COLORMAP);
}

static void cm_set_color(uint32_t cm, uint32_t index, uint32_t rgb32)
{
    uint32_t table = cm_get_colortable(cm);
    uint32_t count = cm_get_count(cm);
    if (!table || index >= count) return;
    m68k_write_memory_32(table + index * 4, rgb32);
}

static uint32_t cm_get_color(uint32_t cm, uint32_t index)
{
    uint32_t table = cm_get_colortable(cm);
    uint32_t count = cm_get_count(cm);
    if (!table || index >= count) return 0;
    return m68k_read_memory_32(table + index * 4);
}

static uint32_t rgb4_to_rgb32(uint8_t r, uint8_t g, uint8_t b)
{
    /* Convert 4-bit components to 32-bit ARGB (alpha = 0xFF). */
    uint8_t rr = (r << 4) | r;
    uint8_t gg = (g << 4) | g;
    uint8_t bb = (b << 4) | b;
    return 0xFF000000 | ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb;
}

static void rgb32_to_rgb4(uint32_t rgb32, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = (uint8_t)(((rgb32 >> 20) & 0x0F));
    *g = (uint8_t)(((rgb32 >> 12) & 0x0F));
    *b = (uint8_t)(((rgb32 >> 4) & 0x0F));
}

static uint32_t rgb32_from_32bit(uint32_t r, uint32_t g, uint32_t b)
{
    /* Clamp to 8-bit and build ARGB. */
    r = r > 0xFF ? 0xFF : r;
    g = g > 0xFF ? 0xFF : g;
    b = b > 0xFF ? 0xFF : b;
    return 0xFF000000 | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
}

/* =========================================================================
 * Default no-op for unimplemented LVOs
 * ========================================================================= */

static void graphics_Unimplemented(void)
{
    /* Unsupported graphics.library LVO — safe no-op.
     * Implemented LVOs are overridden in the table below.
     */
}

/* =========================================================================
 * Implementation
 * ========================================================================= */

static void graphics_InitRastPort(void)
{
    /* InitRastPort — zero out the guest RastPort structure */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    if (!rp) return;
    for (int i = 0; i < RP_SIZE_MIN; i++)
        m68k_write_memory_8(rp + i, 0);
    /* Default pen = 1 (white), draw mode = JAM2 (Workbench default) */
    rp_w_u8(rp, RP_OFF_FGPEN, 1);
    rp_w_u8(rp, RP_OFF_BGPEN, 0);
    rp_w_u8(rp, RP_OFF_DRAWMODE, JAM2);
}

static void graphics_InitView(void)
{
    /* InitView(view) — A1 = view
     * Zero the View structure and mark it as empty.
     */
    uint32_t view = m68k_get_reg(NULL, M68K_REG_A1);
    if (!view) return;
    for (int i = 0; i < 64; i++)
        m68k_write_memory_8(view + i, 0);
}

static void graphics_InitVPort(void)
{
    /* InitVPort(vport) — A1 = vport
     * Zero the ViewPort structure.
     */
    uint32_t vport = m68k_get_reg(NULL, M68K_REG_A1);
    if (!vport) return;
    for (int i = 0; i < 80; i++)
        m68k_write_memory_8(vport + i, 0);
}

static void graphics_InitBitMap(void)
{
    /* InitBitMap(bm, width, height, depth)
     * A1 = bm, D0 = width, D1 = height, D2 = depth
     * Set up the BitMap header and clear plane pointers.
     */
    uint32_t bm = m68k_get_reg(NULL, M68K_REG_A1);
    int w = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int h = (int)m68k_get_reg(NULL, M68K_REG_D1);
    int d = (int)m68k_get_reg(NULL, M68K_REG_D2);
    if (!bm) return;
    uint16_t bytes_per_row = (uint16_t)(((w + 15) / 16) * 2);
    if (bytes_per_row < 2) bytes_per_row = 2;
    m68k_write_memory_16(bm + BM_OFF_BYTESPERROW, bytes_per_row);
    m68k_write_memory_16(bm + BM_OFF_ROWS, (uint16_t)h);
    m68k_write_memory_8(bm + BM_OFF_FLAGS, 0);
    m68k_write_memory_8(bm + BM_OFF_DEPTH, (uint8_t)d);
    for (int i = 0; i < 8; i++)
        m68k_write_memory_32(bm + BM_OFF_PLANES + i * 4, 0);
}

static uint32_t cmap_lookup_rgb(uint32_t cmap, uint32_t pen)
{
    if (!cmap) return amiga_pen_to_rgb((uint8_t)pen);
    uint32_t table = m68k_read_memory_32(cmap + CM_OFF_COLORTABLE);
    uint32_t count = (uint32_t)m68k_read_memory_16(cmap + CM_OFF_COUNT);
    if (!table || pen >= count) return amiga_pen_to_rgb((uint8_t)pen);
    return m68k_read_memory_32(table + pen * 4);
}

void render_bitmap_to_framebuffer(uint32_t bm, uint32_t cmap, int dx, int dy, int w, int h)
{
    if (!bm || !g_fb.valid) return;
    BlitSurface s;
    blit_surface_from_bitmap(&s, bm);
    if (s.is_fb) return;

    /* Source bounds */
    int sx1 = 0, sy1 = 0;
    int sx2 = (w < s.width) ? w - 1 : (int)s.width - 1;
    int sy2 = (h < s.height) ? h - 1 : (int)s.height - 1;

    for (int y = sy1; y <= sy2; y++) {
        int dst_y = dy + y;
        if (dst_y < 0 || dst_y >= (int)g_fb.height) continue;
        for (int x = sx1; x <= sx2; x++) {
            int dst_x = dx + x;
            if (dst_x < 0 || dst_x >= (int)g_fb.width) continue;
            uint32_t pen = 0;
            if (!blit_surface_get(&s, x, y, &pen)) continue;
            uint32_t rgb = cmap_lookup_rgb(cmap, pen);
            FB_PutPixel(dst_x, dst_y, rgb);
        }
    }
}

/* =========================================================================
 * Tier 3 — Copper list construction (MakeVPort / MrgCop / LoadView)
 * ========================================================================= */

/* Amiga custom register offsets (relative to 0xDFF000) used in copper MOVEs. */
#define COP_DMACON   0x096
#define COP_BPLCON0  0x100
#define COP_BPLCON1  0x102
#define COP_BPLCON2  0x104
#define COP_BPLCON3  0x106
#define COP_BPLCON4  0x10C
#define COP_BPL1MOD  0x108
#define COP_BPL2MOD  0x10A
#define COP_DIWSTART 0x08E
#define COP_DIWSTOP  0x090
#define COP_DDFSTART 0x092
#define COP_DDFSTOP  0x094
#define COP_BPL1PT   0x0E0
#define COP_BPL2PT   0x0E4
#define COP_BPL3PT   0x0E8
#define COP_BPL4PT   0x0EC
#define COP_BPL5PT   0x0F0
#define COP_BPL6PT   0x0F4
#define COP_COLOR00  0x180

/* Write one big-endian 16-bit copper word. */
static void cop_w16(uint32_t addr, uint16_t v)
{
    m68k_write_memory_16(addr, v);
}

/* Write a copper MOVE instruction (4 bytes). */
static void cop_move(uint32_t *p, uint16_t reg, uint16_t data)
{
    cop_w16(*p, reg & 0xFFFEu);
    cop_w16(*p + 2, data);
    *p += 4;
}

/* Write a copper WAIT instruction. */
static void cop_wait(uint32_t *p, uint16_t vp, uint16_t hp, uint16_t mask)
{
    cop_w16(*p, (uint16_t)(0x0001u | ((vp & 0xFFu) << 8) | ((hp & 0x7Fu) << 1)));
    cop_w16(*p + 2, (uint16_t)(0x0001u | ((mask & 0xFFu) << 8) | ((mask & 0x7Fu) << 1)));
    *p += 4;
}

/* End-of-copper-list: WAIT $FFFEFFFE. */
static void cop_end(uint32_t *p)
{
    cop_w16(*p, 0xFFFEu);
    cop_w16(*p + 2, 0xFFFEu);
    *p += 4;
}

#define MAX_VIEWPORTS 8
#define MAX_COPPER_SIZE 512

static uint32_t s_vp_copper[MAX_VIEWPORTS];
static uint32_t s_view_copper = 0;

static int vp_index(uint32_t vp)
{
    for (int i = 0; i < MAX_VIEWPORTS; i++)
        if (s_vp_copper[i] == vp) return i;
    for (int i = 0; i < MAX_VIEWPORTS; i++) {
        if (s_vp_copper[i] == 0) { s_vp_copper[i] = vp; return i; }
    }
    return -1;
}

/* Build a copper list for a single ViewPort.  Returns guest address or 0. */
static uint32_t build_viewport_copper(uint32_t vp)
{
    if (!vp) return 0;
    uint32_t rasinfo = m68k_read_memory_32(vp + VP_OFF_RASINFO);
    if (!rasinfo) return 0;
    uint32_t bm = m68k_read_memory_32(rasinfo + RI_OFF_BITMAP);
    if (!bm) return 0;

    uint32_t cmap = m68k_read_memory_32(vp + VP_OFF_COLORMAP);
    uint32_t mode_id = m68k_read_memory_32(vp + VP_OFF_MODES);
    uint32_t di = disp_find_mode(mode_id);

    int depth = (int)m68k_read_memory_8(bm + BM_OFF_DEPTH);
    if (depth < 1 || depth > 8) depth = 5;
    int width  = (int)m68k_read_memory_16(vp + VP_OFF_DWIDTH);
    int height = (int)m68k_read_memory_16(vp + VP_OFF_DHEIGHT);
    if (width <= 0)  width = di ? (int)m68k_read_memory_16(di + DI_WIDTH)  : 320;
    if (height <= 0) height = di ? (int)m68k_read_memory_16(di + DI_HEIGHT) : 256;

    /* Allocate chip RAM for the copper list. */
    uint32_t copper = 0;
    dos_AllocMem_glue(MAX_COPPER_SIZE, MEMF_CHIP, &copper);
    if (!copper) return 0;
    for (uint32_t i = 0; i < MAX_COPPER_SIZE; i++) m68k_write_memory_8(copper + i, 0);

    uint32_t p = copper;

    /* Enable bitplane DMA. */
    cop_move(&p, COP_DMACON, 0x8100);

    /* BPLCON0: bitplane depth. */
    uint16_t bplcon0 = (uint16_t)((depth & 0x7) << 12);
    if (mode_id & AGA_FLAG) bplcon0 |= 0x8000; /* HIRES placeholder for AGA modes */
    cop_move(&p, COP_BPLCON0, bplcon0);

    cop_move(&p, COP_BPLCON1, 0);
    cop_move(&p, COP_BPLCON2, 0);
    cop_move(&p, COP_BPLCON3, 0);
    cop_move(&p, COP_BPLCON4, 0);

    /* Display window. */
    cop_move(&p, COP_DIWSTART, (uint16_t)((26 << 8) | 0x81));
    cop_move(&p, COP_DIWSTOP,  (uint16_t)((((26 + height) & 0xFF) << 8) | 0xC1));
    cop_move(&p, COP_DDFSTART, 0x38);
    cop_move(&p, COP_DDFSTOP,  0xD0);

    /* Modulos: default 0. */
    cop_move(&p, COP_BPL1MOD, 0);
    cop_move(&p, COP_BPL2MOD, 0);

    /* Bitplane pointers. */
    for (int i = 0; i < depth && i < 8; i++) {
        uint32_t pt = m68k_read_memory_32(bm + BM_OFF_PLANES + i * 4);
        cop_move(&p, COP_BPL1PT + i * 4, (uint16_t)(pt >> 16));
        cop_move(&p, COP_BPL1PT + i * 4 + 2, (uint16_t)(pt & 0xFFFFu));
    }

    /* Colour registers from the ColorMap. */
    if (cmap) {
        uint32_t table = m68k_read_memory_32(cmap + CM_OFF_COLORTABLE);
        uint32_t count = m68k_read_memory_16(cmap + CM_OFF_COUNT);
        if (table) {
            for (uint32_t i = 0; i < count && i < 32; i++) {
                uint32_t rgb = m68k_read_memory_32(table + i * 4);
                uint32_t r = (rgb >> 16) & 0xFF;
                uint32_t g = (rgb >> 8) & 0xFF;
                uint32_t b = rgb & 0xFF;
                uint16_t aga = (uint16_t)(((r & 0xF0) << 4) | (g & 0xF0) | ((b & 0xF0) >> 4));
                cop_move(&p, COP_COLOR00 + i * 2, aga);
            }
        }
    }

    cop_end(&p);
    (void)width;
    return copper;
}

static void graphics_MakeVPort(void)
{
    /* MakeVPort(view, vp) — A0 = view, A1 = vp
     * Build a copper list for this ViewPort and remember it. */
    uint32_t vp = m68k_get_reg(NULL, M68K_REG_A1);
    int idx = vp_index(vp);
    if (idx < 0) return;

    /* Free any previous copper list. */
    if (s_vp_copper[idx] & 1) {
        /* We stored address+1 in the table to distinguish from 0. */
        uint32_t old = s_vp_copper[idx] & ~1u;
        dos_FreeMem_glue(old, MAX_COPPER_SIZE);
    }

    uint32_t copper = build_viewport_copper(vp);
    if (copper)
        s_vp_copper[idx] = copper | 1u; /* mark as allocated */
    else
        s_vp_copper[idx] = vp | 1u;
}

static void graphics_MrgCop(void)
{
    /* MrgCop(view) — A1 = view
     * Merge all ViewPort copper lists into one master list for the View. */
    uint32_t view = m68k_get_reg(NULL, M68K_REG_A1);
    if (!view) return;

    if (s_view_copper) {
        dos_FreeMem_glue(s_view_copper, MAX_COPPER_SIZE);
        s_view_copper = 0;
    }

    uint32_t copper = 0;
    dos_AllocMem_glue(MAX_COPPER_SIZE, MEMF_CHIP, &copper);
    if (!copper) return;
    for (uint32_t i = 0; i < MAX_COPPER_SIZE; i++) m68k_write_memory_8(copper + i, 0);

    uint32_t p = copper;

    for (uint32_t vp = m68k_read_memory_32(view + VIEW_OFF_VIEWPORT); vp; vp = m68k_read_memory_32(vp + VP_OFF_NEXT)) {
        int idx = vp_index(vp);
        if (idx < 0) continue;
        uint32_t vp_copper = s_vp_copper[idx] & ~1u;
        if (!vp_copper) continue;

        /* Copy the ViewPort copper list into the master list.
         * Stop when we hit the end WAIT ($FFFE,$FFFE). */
        uint32_t src = vp_copper;
        for (uint32_t n = 0; n < MAX_COPPER_SIZE / 4; n++) {
            uint16_t w1 = (uint16_t)m68k_read_memory_16(src);
            uint16_t w2 = (uint16_t)m68k_read_memory_16(src + 2);
            cop_w16(p, w1);
            cop_w16(p + 2, w2);
            p += 4;
            src += 4;
            if (w1 == 0xFFFEu && w2 == 0xFFFEu) break;
        }
    }

    if (p == copper) {
        /* No ViewPorts: just an end instruction. */
        cop_end(&p);
    }

    s_view_copper = copper;
}

static void graphics_LoadView(void)
{
    /* LoadView(view) — A1 = view
     * If a merged copper list has been built for this View, execute it through
     * the AGA chipset emulator and render the resulting display state.  If not,
     * fall back to the CPU-drawn bitmap path.
     */
    uint32_t view = m68k_get_reg(NULL, M68K_REG_A1);
    if (!view) {
        FB_FillRect(0, 0, (int)g_fb.width, (int)g_fb.height, 0);
        return;
    }

    /* If MrgCop has built a copper list, use the chipset renderer. */
    if (s_view_copper) {
        chip_emu_copper_jump(1, s_view_copper); /* sets COP1LC and executes it */
        chip_emu_render_frame();
        return;
    }

    /* Fallback CPU-drawn path for Views without a copper list. */
    for (uint32_t vp = m68k_read_memory_32(view + VIEW_OFF_VIEWPORT); vp; vp = m68k_read_memory_32(vp + VP_OFF_NEXT)) {
        uint32_t rasinfo = m68k_read_memory_32(vp + VP_OFF_RASINFO);
        uint32_t cmap    = m68k_read_memory_32(vp + VP_OFF_COLORMAP);
        if (!rasinfo) continue;

        uint32_t bm = m68k_read_memory_32(rasinfo + RI_OFF_BITMAP);
        int dx = (int)m68k_read_memory_16(vp + VP_OFF_DXOFFSET);
        int dy = (int)m68k_read_memory_16(vp + VP_OFF_DYOFFSET);
        int w  = (int)m68k_read_memory_16(vp + VP_OFF_DWIDTH);
        int h  = (int)m68k_read_memory_16(vp + VP_OFF_DHEIGHT);

        render_bitmap_to_framebuffer(bm, cmap, dx, dy, w, h);
    }
}

static void graphics_WaitTOF(void)
{
    /* WaitTOF — wait for the next VBlank tick from the chipset emulator.
     * M68k tasks get a dedicated VBlank signal bit so they can block on
     * Wait() instead of busy-waiting and starving the idle/WM task.
     * After waking, trigger a WM redraw so any bitmap changes made by
     * the guest (e.g. WritePixelArray8 into a SuperBitMap) are rendered
     * to the framebuffer on the next vertical blank. */
    UaosTask *t = Task_Current();
    if (t && t->type == TASK_TYPE_M68K && t->m68k_vblank_sig >= 0) {
        uint32_t sigmask = (1u << (unsigned int)t->m68k_vblank_sig);
        /* Clear any already-pending VBlank signal so we wait for the next one. */
        t->tc_SigRecvd &= ~sigmask;
        g_wait_tof_task = t;
        Wait(sigmask);
        g_wait_tof_task = NULL;
        WM_Redraw();
        return;
    }

    /* Fallback for native callers / tasks without a VBlank signal bit. */
    uint32_t start = chip_emu_vblank_count();
    while (chip_emu_vblank_count() == start) {
        __asm__ volatile ("pause");
    }
}

static void graphics_ChangeVPBitMap(void)
{
    /* ChangeVPBitMap(vp, bm, rasinfo)
     * A0 = vp, A1 = bm, A2 = rasinfo
     * Swap the RasInfo bitmap pointer in a ViewPort without rebuilding the
     * whole display.  If rasinfo is NULL the ViewPort's current RasInfo is
     * used.  The new BitMap is assumed to have the same depth/geometry as
     * the old one.
     */
    uint32_t vp = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t bm = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t ri = m68k_get_reg(NULL, M68K_REG_A2);
    if (!vp || !bm) return;
    if (!ri) ri = m68k_read_memory_32(vp + VP_OFF_RASINFO);
    if (!ri) return;
    m68k_write_memory_32(ri + RI_OFF_BITMAP, bm);
}

/* Render a single character from the built-in 8×16 font.
 * For JAM1 only foreground pixels are drawn (transparent background).
 * For JAM2 the cell is opaque. */
static void draw_text_char(uint32_t rp, int x, int y, char ch, uint32_t fg, int mode, uint32_t bg)
{
    uint8_t c = (uint8_t)ch;
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *glyph = g_font8x16[c - 0x20];

    if (mode == JAM1) {
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col))
                    rp_put_pixel(rp, x + col, y + row, fg);
            }
        }
    } else {
        /* JAM2: opaque background cell */
        rp_fill_rect(rp, x, y, 8, 16, bg);
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col))
                    rp_put_pixel(rp, x + col, y + row, fg);
            }
        }
    }
}

static void graphics_Text(void)
{
    /* Text(rp, string, length)
     * A1 = rp, A0 = string, D0 = length
     */
    uint32_t rp  = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t str = m68k_get_reg(NULL, M68K_REG_A0);
    int      len = (int)m68k_get_reg(NULL, M68K_REG_D0);
    if (!rp || !str || len <= 0) return;

    int x = (int)rp_s16(rp, RP_OFF_CP_X);
    int y = (int)rp_s16(rp, RP_OFF_CP_Y);

    uint32_t fg = current_fg(rp);
    uint32_t bg = current_bg(rp);
    uint32_t ol = current_outline(rp);
    int mode    = current_mode(rp);
    int outline = (mode & OUTLINE) ? 1 : 0;

    int cw = text_char_width(rp);
    for (int i = 0; i < len; i++) {
        char ch = (char)m68k_read_memory_8(str + i);
        if (outline) {
            /* Draw a simple outline: render the character shifted to all 8
             * neighbours with the outline pen, then draw the glyph normally. */
            for (int oy = -1; oy <= 1; oy++) {
                for (int ox = -1; ox <= 1; ox++) {
                    if (ox == 0 && oy == 0) continue;
                    draw_text_char(rp, x + ox, y + oy, ch, ol, JAM1, bg);
                }
            }
        }
        draw_text_char(rp, x, y, ch, fg, mode, bg);
        x += cw;
    }

    rp_w_s16(rp, RP_OFF_CP_X, (int16_t)x);
}

static void graphics_TextFit(void)
{
    /* TextFit(rp, string, strLen, textExtent, constrainingExtent,
     *         strDirection, constrainingBitWidth, constrainingBitHeight)
     * A1 = rp, A0 = string, D0 = strLen, A2 = textExtent, A3 = constrainingExtent,
     * D1 = strDirection, D2 = width, D3 = height
     * Returns the number of characters that fit in the width constraint.
     */
    uint32_t rp  = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t str = m68k_get_reg(NULL, M68K_REG_A0);
    int      len = (int)m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t te  = m68k_get_reg(NULL, M68K_REG_A2);
    uint32_t ce  = m68k_get_reg(NULL, M68K_REG_A3);
    int      dir = (int)m68k_get_reg(NULL, M68K_REG_D1);
    int      w   = (int)m68k_get_reg(NULL, M68K_REG_D2);
    int      h   = (int)m68k_get_reg(NULL, M68K_REG_D3);
    (void)rp; (void)str; (void)ce; (void)dir; (void)h;

    if (len < 0) len = 0;
    int cw = text_char_width(rp);
    int fit = (w / cw);
    if (fit > len) fit = len;
    if (fit < 0) fit = 0;

    if (te) {
        int baseline = text_baseline(rp);
        fill_text_extent(te, 0, -baseline, fit * cw, text_char_height(rp), baseline);
    }

    m68k_set_reg(M68K_REG_D0, (unsigned int)fit);
}

static void graphics_TextLength(void)
{
    /* TextLength(rp, string, length)
     * A1 = rp, A0 = string, D0 = length
     * Returns pixel width in D0.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int len = (int)m68k_get_reg(NULL, M68K_REG_D0);
    if (len < 0) len = 0;
    m68k_set_reg(M68K_REG_D0, (unsigned int)(len * text_char_width(rp)));
}

static void graphics_OpenFont(void)
{
    /* OpenFont(textAttr)
     * A0 = textAttr
     * Returns a pointer to the font in D0, or 0 on failure.
     */
    uint32_t textAttr = m68k_get_reg(NULL, M68K_REG_A0);
    if (!textAttr) { m68k_set_reg(M68K_REG_D0, 0); return; }

    uint32_t name_ptr = m68k_read_memory_32(textAttr + TA_OFF_NAME);
    uint16_t ySize    = m68k_read_memory_16(textAttr + TA_OFF_YSIZE);
    uint8_t  style    = m68k_read_memory_8 (textAttr + TA_OFF_STYLE);
    uint8_t  flags    = m68k_read_memory_8 (textAttr + TA_OFF_FLAGS);

    char name[32] = {0};
    if (name_ptr) {
        for (int i = 0; i < 31; i++) {
            char c = (char)m68k_read_memory_8(name_ptr + i);
            name[i] = c;
            if (c == 0) break;
        }
    }

    m68k_set_reg(M68K_REG_D0, font_open(name, ySize, style, flags));
}

static void graphics_CloseFont(void)
{
    /* CloseFont(font)
     * A1 = font
     */
    uint32_t font = m68k_get_reg(NULL, M68K_REG_A1);
    font_close(font);
}

static void graphics_AddFont(void)
{
    /* AddFont(font) — A1 = font
     * Registers a font in the database.  UAOS keeps a static database, so
     * this is only a no-op if the font is already known.
     */
    uint32_t font = m68k_get_reg(NULL, M68K_REG_A1);
    if (font && font_is_known(font)) return;
    (void)font;
}

static void graphics_RemFont(void)
{
    /* RemFont(font) — A1 = font
     * Removes a font from the database.  Static fonts are never removed.
     */
    uint32_t font = m68k_get_reg(NULL, M68K_REG_A1);
    (void)font;
}

static void graphics_TextExtent(void)
{
    /* TextExtent(rp, string, length, textExtent)
     * A1 = rp, A0 = string, D0 = length, A2 = textExtent
     */
    uint32_t rp  = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t str = m68k_get_reg(NULL, M68K_REG_A0);
    int      len = (int)m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t te  = m68k_get_reg(NULL, M68K_REG_A2);

    int w = text_width(rp, str, len);
    int h = text_height(rp);
    int baseline = text_baseline(rp);
    fill_text_extent(te, 0, -baseline, w, h, baseline);

    m68k_set_reg(M68K_REG_D0, (unsigned int)w);
}

static void graphics_FontExtent(void)
{
    /* FontExtent(font, textExtent)
     * A0 = font, A1 = textExtent
     */
    uint32_t font = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t te   = m68k_get_reg(NULL, M68K_REG_A1);
    if (!font || !te) { m68k_set_reg(M68K_REG_D0, 0); return; }

    int xsize = (int)m68k_read_memory_16(font + TF_OFF_XSIZE);
    int ysize = (int)m68k_read_memory_16(font + TF_OFF_YSIZE);
    int baseline = (int)m68k_read_memory_16(font + TF_OFF_BASELINE);
    fill_text_extent(te, 0, -baseline, xsize, ysize, baseline);

    m68k_set_reg(M68K_REG_D0, (unsigned int)xsize);
}

static void graphics_AskSoftStyle(void)
{
    /* AskSoftStyle(rp) — A1 = rp
     * Returns the current soft style in D0.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t style = rp ? rp_u8(rp, RP_OFF_SOFTSTYLE) : 0;
    m68k_set_reg(M68K_REG_D0, style);
}

static void graphics_SetSoftStyle(void)
{
    /* SetSoftStyle(rp, newStyle, enableFlags)
     * A1 = rp, D0 = newStyle, D1 = enableFlags
     * Returns the previous soft style in D0.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint8_t newStyle = (uint8_t)m68k_get_reg(NULL, M68K_REG_D0);
    uint8_t enableFlags = (uint8_t)m68k_get_reg(NULL, M68K_REG_D1);
    if (!rp) { m68k_set_reg(M68K_REG_D0, 0); return; }

    uint8_t old = rp_u8(rp, RP_OFF_SOFTSTYLE);
    uint8_t result = (old & ~enableFlags) | (newStyle & enableFlags);
    rp_w_u8(rp, RP_OFF_SOFTSTYLE, result);
    m68k_set_reg(M68K_REG_D0, old);
}

static void graphics_ExtendFont(void)
{
    /* ExtendFont(font, tags) — A0 = font, A1 = tags
     * Returns TRUE (1) for any known font, FALSE (0) on failure.
     */
    uint32_t font = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t tags = m68k_get_reg(NULL, M68K_REG_A1);
    (void)tags;
    m68k_set_reg(M68K_REG_D0, font_is_known(font) ? 1 : 0);
}

static void graphics_StripFont(void)
{
    /* StripFont(font) — A0 = font
     * Removes extended font data; no-op for the built-in font.
     */
    (void)m68k_get_reg(NULL, M68K_REG_A0);
}

static void graphics_Move(void)
{
    /* Move(rp, x, y)
     * A1 = rp, D0 = x, D1 = y
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int x = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int y = (int)m68k_get_reg(NULL, M68K_REG_D1);
    if (!rp) return;
    rp_w_s16(rp, RP_OFF_CP_X, (int16_t)x);
    rp_w_s16(rp, RP_OFF_CP_Y, (int16_t)y);
}

static void graphics_Draw(void)
{
    /* Draw(rp, x, y)
     * A1 = rp, D0 = x, D1 = y
     * Draw line from current pen position to (x,y), then update pen pos.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int x1 = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int y1 = (int)m68k_get_reg(NULL, M68K_REG_D1);
    if (!rp) return;

    int x0 = (int)rp_s16(rp, RP_OFF_CP_X);
    int y0 = (int)rp_s16(rp, RP_OFF_CP_Y);

    uint32_t col = current_fg(rp);
    draw_line(rp, x0, y0, x1, y1, col);

    rp_w_s16(rp, RP_OFF_CP_X, (int16_t)x1);
    rp_w_s16(rp, RP_OFF_CP_Y, (int16_t)y1);
}

static void graphics_RectFill(void)
{
    /* RectFill(rp, xMin, yMin, xMax, yMax)
     * A1 = rp, D0 = xMin, D1 = yMin, D2 = xMax, D3 = yMax
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int x1 = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int y1 = (int)m68k_get_reg(NULL, M68K_REG_D1);
    int x2 = (int)m68k_get_reg(NULL, M68K_REG_D2);
    int y2 = (int)m68k_get_reg(NULL, M68K_REG_D3);

    uint32_t col = current_fg(rp);
    rp_fill_rect(rp, x1, y1, x2 - x1 + 1, y2 - y1 + 1, col);
}

static void graphics_EraseRect(void)
{
    /* EraseRect(rp, xMin, yMin, xMax, yMax)
     * A1 = rp, D0 = xMin, D1 = yMin, D2 = xMax, D3 = yMax
     * Fills the rectangle with the background pen.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int x1 = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int y1 = (int)m68k_get_reg(NULL, M68K_REG_D1);
    int x2 = (int)m68k_get_reg(NULL, M68K_REG_D2);
    int y2 = (int)m68k_get_reg(NULL, M68K_REG_D3);
    uint32_t bg = current_bg(rp);
    rp_fill_rect(rp, x1, y1, x2 - x1 + 1, y2 - y1 + 1, bg);
}

static void graphics_ClearEOL(void)
{
    /* ClearEOL(rp) — A1 = rp
     * Clears from the current pen position to the right edge of the line.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    if (!rp) return;
    int x = (int)rp_s16(rp, RP_OFF_CP_X);
    int y = (int)rp_s16(rp, RP_OFF_CP_Y);
    uint32_t bg = current_bg(rp);
    BlitSurface s;
    blit_surface_from_rastport(&s, rp);
    int max_x = s.is_fb ? (int)g_fb.width : s.width;
    rp_fill_rect(rp, x, y, max_x - x, text_char_height(rp), bg);
}

static void graphics_ClearScreen(void)
{
    /* ClearScreen(rp) — A1 = rp
     * Clears the entire surface with the background pen.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t bg = current_bg(rp);
    BlitSurface s;
    blit_surface_from_rastport(&s, rp);
    if (s.is_fb) {
        if (!g_fb.valid) return;
        FB_FillRect(0, 0, (int)g_fb.width, (int)g_fb.height, rp_pen_to_rgb(rp, bg));
    } else {
        rp_fill_rect(rp, 0, 0, s.width, s.height, bg);
    }
}

static void graphics_DrawEllipse(void)
{
    /* DrawEllipse(rp, x, y, rx, ry)
     * A1 = rp, D0 = x, D1 = y, D2 = rx, D3 = ry
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int cx = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int cy = (int)m68k_get_reg(NULL, M68K_REG_D1);
    int rx = (int)m68k_get_reg(NULL, M68K_REG_D2);
    int ry = (int)m68k_get_reg(NULL, M68K_REG_D3);
    uint32_t col = current_fg(rp);
    draw_ellipse(rp, cx, cy, rx, ry, col, 0);
}

static void graphics_AreaEllipse(void)
{
    /* AreaEllipse(rp, x, y, rx, ry)
     * A1 = rp, D0 = x, D1 = y, D2 = rx, D3 = ry
     * Filled ellipse; also records the outline for area-fill if used.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int cx = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int cy = (int)m68k_get_reg(NULL, M68K_REG_D1);
    int rx = (int)m68k_get_reg(NULL, M68K_REG_D2);
    int ry = (int)m68k_get_reg(NULL, M68K_REG_D3);
    uint32_t col = current_fg(rp);
    draw_ellipse(rp, cx, cy, rx, ry, col, 1);
}

static void graphics_PolyDraw(void)
{
    /* PolyDraw(rp, count, array)
     * A1 = rp, D0 = count, A0 = array of XY WORDs
     * Draws lines from the current pen position to the first point, then
     * between successive points, and updates the pen position.
     */
    uint32_t rp  = m68k_get_reg(NULL, M68K_REG_A1);
    int      cnt = (int)m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t arr = m68k_get_reg(NULL, M68K_REG_A0);
    if (!rp || cnt < 1 || !arr) return;

    int x0 = (int)rp_s16(rp, RP_OFF_CP_X);
    int y0 = (int)rp_s16(rp, RP_OFF_CP_Y);
    uint32_t col = current_fg(rp);

    for (int i = 0; i < cnt; i++) {
        int x1 = (int)m68k_read_memory_16(arr + i * 4);
        int y1 = (int)m68k_read_memory_16(arr + i * 4 + 2);
        draw_line(rp, x0, y0, x1, y1, col);
        x0 = x1; y0 = y1;
    }

    rp_w_s16(rp, RP_OFF_CP_X, (int16_t)x0);
    rp_w_s16(rp, RP_OFF_CP_Y, (int16_t)y0);
}

static void graphics_Flood(void)
{
    /* Flood(rp, x, y)
     * A1 = rp, D0 = x, D1 = y
     * 4-way flood fill starting at (x,y), replacing the colour under the
     * start point with the foreground pen.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int x = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int y = (int)m68k_get_reg(NULL, M68K_REG_D1);
    if (!rp) return;
    BlitSurface s;
    blit_surface_from_rastport(&s, rp);
    uint32_t target = 0;
    if (!blit_surface_get(&s, x, y, &target)) return;
    uint32_t fill = current_fg(rp);
    if (target != fill) flood_fill(rp, x, y, target, fill);
}

static void graphics_BltClear(void)
{
    /* BltClear(mem, byteSize, flags)
     * A0 = memory, D0 = byteSize, D1 = flags
     * Zeroes the supplied memory block.  Flags are ignored.
     */
    uint32_t mem = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t size = m68k_get_reg(NULL, M68K_REG_D0);
    (void)m68k_get_reg(NULL, M68K_REG_D1);
    if (!mem || size == 0) return;
    for (uint32_t i = 0; i < size; i++)
        m68k_write_memory_8(mem + i, 0);
}

static void graphics_NewRegion(void)
{
    /* NewRegion() — returns a pointer to an empty Region in D0. */
    uint32_t rg = 0, rr = 0;
    dos_AllocMem_glue(RG_SIZE, 0, &rg);
    if (!rg) { m68k_set_reg(M68K_REG_D0, 0); return; }
    dos_AllocMem_glue(RR_SIZE, 0, &rr);
    if (!rr) {
        dos_FreeMem_glue(rg, RG_SIZE);
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    for (int i = 0; i < RG_SIZE; i++) m68k_write_memory_8(rg + i, 0);
    for (int i = 0; i < RR_SIZE; i++) m68k_write_memory_8(rr + i, 0);

    /* Empty region: MinX > MaxX */
    rg_set_bounds(rg, 0, 0, -1, -1);
    m68k_write_memory_32(rg + RG_OFF_REGIONRECT, rr);
    m68k_write_memory_32(rr + RR_OFF_NEXT, 0);
    m68k_write_memory_32(rr + RR_OFF_PREV, 0);
    m68k_set_reg(M68K_REG_D0, rg);
}

static void graphics_DisposeRegion(void)
{
    /* DisposeRegion(region) — A0 = region */
    uint32_t rg = m68k_get_reg(NULL, M68K_REG_A0);
    if (!rg) return;
    uint32_t rr = m68k_read_memory_32(rg + RG_OFF_REGIONRECT);
    while (rr) {
        uint32_t next = m68k_read_memory_32(rr + RR_OFF_NEXT);
        dos_FreeMem_glue(rr, RR_SIZE);
        rr = next;
    }
    dos_FreeMem_glue(rg, RG_SIZE);
}

static void graphics_AndRectRegion(void)
{
    /* AndRectRegion(region, rectangle) — A0 = region, A1 = rectangle
     * Returns TRUE (1) if the result is non-empty, FALSE (0) otherwise.
     */
    uint32_t rg = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t rect = m68k_get_reg(NULL, M68K_REG_A1);
    if (!rg || !rect) { m68k_set_reg(M68K_REG_D0, 0); return; }

    int16_t rminx, rminy, rmaxx, rmaxy;
    int16_t minx, miny, maxx, maxy;
    rg_read_bounds(rg, &rminx, &rminy, &rmaxx, &rmaxy);
    rg_read_rect(rect, &minx, &miny, &maxx, &maxy);

    if (rminx > rmaxx || rminy > rmaxy) {
        /* region already empty */
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    int16_t nx, ny, nw, nh;
    rg_intersect_rect(rminx, rminy, rmaxx, rmaxy,
                      minx, miny, maxx, maxy,
                      &nx, &ny, &nw, &nh);
    rg_set_bounds(rg, nx, ny, nw, nh);
    m68k_set_reg(M68K_REG_D0, (nx <= nw && ny <= nh) ? 1 : 0);
}

static void graphics_OrRectRegion(void)
{
    /* OrRectRegion(region, rectangle) — A0 = region, A1 = rectangle
     * Returns TRUE (1) if the result is non-empty.
     */
    uint32_t rg = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t rect = m68k_get_reg(NULL, M68K_REG_A1);
    if (!rg || !rect) { m68k_set_reg(M68K_REG_D0, 0); return; }

    int16_t rminx, rminy, rmaxx, rmaxy;
    int16_t minx, miny, maxx, maxy;
    rg_read_bounds(rg, &rminx, &rminy, &rmaxx, &rmaxy);
    rg_read_rect(rect, &minx, &miny, &maxx, &maxy);

    int16_t nx, ny, nw, nh;
    rg_union_rect(rminx, rminy, rmaxx, rmaxy,
                  minx, miny, maxx, maxy,
                  &nx, &ny, &nw, &nh);
    rg_set_bounds(rg, nx, ny, nw, nh);
    m68k_set_reg(M68K_REG_D0, (nx <= nw && ny <= nh) ? 1 : 0);
}

static void graphics_XorRectRegion(void)
{
    /* XorRectRegion(region, rectangle) — A0 = region, A1 = rectangle
     * For a single-bounding-box representation, geometric XOR is not
     * generally rectangular; we approximate as the union.
     */
    graphics_OrRectRegion();
}

static void graphics_ClearRectRegion(void)
{
    /* ClearRectRegion(region, rectangle) — A0 = region, A1 = rectangle
     * Returns TRUE if the region is still non-empty.
     */
    uint32_t rg = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t rect = m68k_get_reg(NULL, M68K_REG_A1);
    if (!rg || !rect) { m68k_set_reg(M68K_REG_D0, 0); return; }

    int16_t rminx, rminy, rmaxx, rmaxy;
    int16_t minx, miny, maxx, maxy;
    rg_read_bounds(rg, &rminx, &rminy, &rmaxx, &rmaxy);
    rg_read_rect(rect, &minx, &miny, &maxx, &maxy);

    /* If the rectangle fully covers the region, empty it. */
    if (minx <= rminx && miny <= rminy && maxx >= rmaxx && maxy >= rmaxy) {
        rg_set_bounds(rg, 0, 0, -1, -1);
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    /* Otherwise, a single bounding box cannot represent the subtraction;
     * keep the region unchanged. */
    m68k_set_reg(M68K_REG_D0, (rminx <= rmaxx && rminy <= rmaxy) ? 1 : 0);
}

static void graphics_ClearRegion(void)
{
    /* ClearRegion(region) — A0 = region */
    uint32_t rg = m68k_get_reg(NULL, M68K_REG_A0);
    if (rg) rg_set_bounds(rg, 0, 0, -1, -1);
}

static void graphics_AndRegionRegion(void)
{
    /* AndRegionRegion(srcRegion, dstRegion) — A0 = src, A1 = dst */
    uint32_t src = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t dst = m68k_get_reg(NULL, M68K_REG_A1);
    if (!src || !dst) { m68k_set_reg(M68K_REG_D0, 0); return; }

    int16_t sx, sy, sw, sh, dx, dy, dw, dh, rx, ry, rw, rh;
    rg_read_bounds(src, &sx, &sy, &sw, &sh);
    rg_read_bounds(dst, &dx, &dy, &dw, &dh);
    rg_intersect_rect(sx, sy, sw, sh, dx, dy, dw, dh, &rx, &ry, &rw, &rh);
    rg_set_bounds(dst, rx, ry, rw, rh);
    m68k_set_reg(M68K_REG_D0, (rx <= rw && ry <= rh) ? 1 : 0);
}

static void graphics_OrRegionRegion(void)
{
    /* OrRegionRegion(srcRegion, dstRegion) — A0 = src, A1 = dst */
    uint32_t src = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t dst = m68k_get_reg(NULL, M68K_REG_A1);
    if (!src || !dst) { m68k_set_reg(M68K_REG_D0, 0); return; }

    int16_t sx, sy, sw, sh, dx, dy, dw, dh, rx, ry, rw, rh;
    rg_read_bounds(src, &sx, &sy, &sw, &sh);
    rg_read_bounds(dst, &dx, &dy, &dw, &dh);
    rg_union_rect(sx, sy, sw, sh, dx, dy, dw, dh, &rx, &ry, &rw, &rh);
    rg_set_bounds(dst, rx, ry, rw, rh);
    m68k_set_reg(M68K_REG_D0, (rx <= rw && ry <= rh) ? 1 : 0);
}

static void graphics_XorRegionRegion(void)
{
    /* XorRegionRegion(srcRegion, dstRegion) — A0 = src, A1 = dst
     * Approximated as a union for the single-bounding-box representation.
     */
    graphics_OrRegionRegion();
}

static void graphics_InitArea(void)
{
    /* InitArea(areaInfo, buffer, maxPoints)
     * A0 = areaInfo, A1 = buffer, D0 = maxPoints
     * UAOS keeps polygon state internally keyed by RastPort, so this is a
     * no-op beyond validating arguments.
     */
    (void)m68k_get_reg(NULL, M68K_REG_A0);
    (void)m68k_get_reg(NULL, M68K_REG_A1);
    (void)m68k_get_reg(NULL, M68K_REG_D0);
}

static void graphics_AreaMove(void)
{
    /* AreaMove(rp, x, y) — A1 = rp, D0 = x, D1 = y
     * Starts a new polygon path.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int x = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int y = (int)m68k_get_reg(NULL, M68K_REG_D1);
    PolygonState *p = poly_get(rp);
    if (!p) return;
    p->count = 0;
    if (p->count < MAX_POLY_POINTS) {
        p->points[p->count][0] = x;
        p->points[p->count][1] = y;
        p->count++;
    }
}

static void graphics_AreaDraw(void)
{
    /* AreaDraw(rp, x, y) — A1 = rp, D0 = x, D1 = y
     * Adds a line to the current polygon path and draws it.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int x = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int y = (int)m68k_get_reg(NULL, M68K_REG_D1);
    PolygonState *p = poly_get(rp);
    if (!p || p->count == 0) return;

    int x0 = p->points[p->count - 1][0];
    int y0 = p->points[p->count - 1][1];
    draw_line(rp, x0, y0, x, y, current_fg(rp));

    if (p->count < MAX_POLY_POINTS) {
        p->points[p->count][0] = x;
        p->points[p->count][1] = y;
        p->count++;
    }
}

static void graphics_AreaEnd(void)
{
    /* AreaEnd(rp) — A1 = rp
     * Closes the polygon path and fills the interior with the foreground pen.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    PolygonState *p = poly_get(rp);
    if (!p || p->count < 3) return;

    int x0 = p->points[p->count - 1][0];
    int y0 = p->points[p->count - 1][1];
    int x1 = p->points[0][0];
    int y1 = p->points[0][1];
    draw_line(rp, x0, y0, x1, y1, current_fg(rp));
    fill_polygon(rp, p, current_fg(rp));
    p->count = 0;
}

static void graphics_SetRast(void)
{
    /* SetRast(rp, pen) — A1 = rp, D0 = pen
     * Fill the RastPort's surface with pen.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint8_t pen = (uint8_t)m68k_get_reg(NULL, M68K_REG_D0);
    if (!rp) return;

    BlitSurface s;
    blit_surface_from_rastport(&s, rp);
    if (s.is_fb) {
        if (g_fb.valid)
            FB_FillRect(0, 0, (int)g_fb.width, (int)g_fb.height, rp_pen_to_rgb(rp, pen));
    } else {
        rp_fill_rect(rp, 0, 0, s.width, s.height, pen);
    }
}

static void graphics_SetAPen(void)
{
    /* SetAPen(rp, pen) — A1 = rp, D0 = pen */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint8_t pen = (uint8_t)m68k_get_reg(NULL, M68K_REG_D0);
    if (rp) rp_w_u8(rp, RP_OFF_FGPEN, pen);
}

static void graphics_SetBPen(void)
{
    /* SetBPen(rp, pen) — A1 = rp, D0 = pen */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint8_t pen = (uint8_t)m68k_get_reg(NULL, M68K_REG_D0);
    if (rp) rp_w_u8(rp, RP_OFF_BGPEN, pen);
}

static void graphics_SetDrMd(void)
{
    /* SetDrMd(rp, mode) — A1 = rp, D0 = mode */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint8_t mode = (uint8_t)m68k_get_reg(NULL, M68K_REG_D0);
    if (rp) rp_w_u8(rp, RP_OFF_DRAWMODE, mode);
}

static void graphics_SetWriteMask(void)
{
    /* SetWriteMask(rp, mask) — A1 = rp, D0 = mask
     * Store the write mask in the RastPort.  Our framebuffer
     * renderer is currently chunky/truecolour, so masking is a
     * no-op for drawing, but we keep the state for correctness.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint8_t mask = (uint8_t)m68k_get_reg(NULL, M68K_REG_D0);
    if (rp) rp_w_u8(rp, RP_OFF_MASK, mask);
}

static void graphics_BltBitMap(void)
{
    /* BltBitMap(src, xSrc, ySrc, dst, xDst, yDst, xSize, ySize, minterm, mask, temp)
     * A0 = src, D0 = xSrc, D1 = ySrc, A1 = dst, D2 = xDst, D3 = yDst,
     * D4 = xSize, D5 = ySize, D6 = minterm, A2 = mask, A3 = temp
     */
    uint32_t src_bm = m68k_get_reg(NULL, M68K_REG_A0);
    int sx = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int sy = (int)m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t dst_bm = m68k_get_reg(NULL, M68K_REG_A1);
    int dx = (int)m68k_get_reg(NULL, M68K_REG_D2);
    int dy = (int)m68k_get_reg(NULL, M68K_REG_D3);
    int w = (int)m68k_get_reg(NULL, M68K_REG_D4);
    int h = (int)m68k_get_reg(NULL, M68K_REG_D5);
    uint8_t minterm = (uint8_t)m68k_get_reg(NULL, M68K_REG_D6);
    uint32_t mask = m68k_get_reg(NULL, M68K_REG_A2);
    (void)m68k_get_reg(NULL, M68K_REG_A3);

    BlitSurface src, dst;
    blit_surface_from_bitmap(&src, src_bm);
    blit_surface_from_bitmap(&dst, dst_bm);

    int mask_bpr = 0;
    if (mask) {
        /* Planar mask: assume same width as source, 2 bytes per 16 pixels. */
        mask_bpr = (w + 15) / 16 * 2;
    }
    blit_rect(&src, sx, sy, &dst, dx, dy, w, h, minterm, mask, mask_bpr, 0, 0);
}

static void graphics_BltTemplate(void)
{
    /* BltTemplate(source, xSrc, srcMod, destRP, xDest, yDest, xSize, ySize)
     * A0 = source, D0 = xSrc, D1 = srcMod, A1 = destRP, D2 = xDest,
     * D3 = yDest, D4 = xSize, D5 = ySize
     * The 1-bit template acts as a mask: set bits draw FgPen, unset bits
     * draw BgPen for JAM2 or are skipped for JAM1.
     */
    uint32_t src = m68k_get_reg(NULL, M68K_REG_A0);
    int sx = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int src_mod = (int)m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int dx = (int)m68k_get_reg(NULL, M68K_REG_D2);
    int dy = (int)m68k_get_reg(NULL, M68K_REG_D3);
    int w = (int)m68k_get_reg(NULL, M68K_REG_D4);
    int h = (int)m68k_get_reg(NULL, M68K_REG_D5);

    BlitSurface dst;
    blit_surface_from_rastport(&dst, rp);
    uint32_t fg = current_fg(rp);
    uint32_t bg = current_bg(rp);
    int mode = current_mode(rp);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int bit_set = planar_mask_get(src, src_mod, sx + x, y);
            uint32_t col = bit_set ? fg : bg;
            if (bit_set || mode == JAM2)
                blit_surface_put(&dst, dx + x, dy + y, col);
        }
    }
}

static void graphics_BltPattern(void)
{
    /* BltPattern(rp, mask, xMin, yMin, xMax, yMax, xOffset, yOffset)
     * A1 = rp, A0 = mask, D0 = xMin, D1 = yMin, D2 = xMax, D3 = yMax,
     * D4 = xOffset, D5 = yOffset
     * Fills the rectangle with a 16×16 tiled 1-bit pattern.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t mask = m68k_get_reg(NULL, M68K_REG_A0);
    int x1 = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int y1 = (int)m68k_get_reg(NULL, M68K_REG_D1);
    int x2 = (int)m68k_get_reg(NULL, M68K_REG_D2);
    int y2 = (int)m68k_get_reg(NULL, M68K_REG_D3);
    int xoff = (int)m68k_get_reg(NULL, M68K_REG_D4);
    int yoff = (int)m68k_get_reg(NULL, M68K_REG_D5);

    if (!rp || !mask) return;
    BlitSurface dst;
    blit_surface_from_rastport(&dst, rp);
    uint32_t fg = current_fg(rp);
    uint32_t bg = current_bg(rp);
    int mode = current_mode(rp);
    int pat_bpr = 2; /* 16-bit wide pattern */

    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            int px = ((x - x1) + xoff) & 15;
            int py = ((y - y1) + yoff) & 15;
            int bit_set = planar_mask_get(mask, pat_bpr, px, py);
            uint32_t col = bit_set ? fg : bg;
            if (bit_set || mode == JAM2)
                blit_surface_put(&dst, x, y, col);
        }
    }
}

static void graphics_ClipBlt(void)
{
    /* ClipBlt(srcRP, xSrc, ySrc, dstRP, xDst, yDst, xSize, ySize, minterm)
     * A0 = srcRP, D0 = xSrc, D1 = ySrc, A1 = dstRP, D2 = xDst, D3 = yDst,
     * D4 = xSize, D5 = ySize, D6 = minterm
     */
    uint32_t src_rp = m68k_get_reg(NULL, M68K_REG_A0);
    int sx = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int sy = (int)m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t dst_rp = m68k_get_reg(NULL, M68K_REG_A1);
    int dx = (int)m68k_get_reg(NULL, M68K_REG_D2);
    int dy = (int)m68k_get_reg(NULL, M68K_REG_D3);
    int w = (int)m68k_get_reg(NULL, M68K_REG_D4);
    int h = (int)m68k_get_reg(NULL, M68K_REG_D5);
    uint8_t minterm = (uint8_t)m68k_get_reg(NULL, M68K_REG_D6);

    BlitSurface src, dst;
    blit_surface_from_rastport(&src, src_rp);
    blit_surface_from_rastport(&dst, dst_rp);
    blit_rect(&src, sx, sy, &dst, dx, dy, w, h, minterm, 0, 0, 0, 0);
}

static void graphics_BltBitMapRastPort(void)
{
    /* BltBitMapRastPort(src, xSrc, ySrc, dstRP, xDst, yDst, xSize, ySize, minterm)
     * A0 = src, D0 = xSrc, D1 = ySrc, A1 = dstRP, D2 = xDst, D3 = yDst,
     * D4 = xSize, D5 = ySize, D6 = minterm
     */
    uint32_t src_bm = m68k_get_reg(NULL, M68K_REG_A0);
    int sx = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int sy = (int)m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t dst_rp = m68k_get_reg(NULL, M68K_REG_A1);
    int dx = (int)m68k_get_reg(NULL, M68K_REG_D2);
    int dy = (int)m68k_get_reg(NULL, M68K_REG_D3);
    int w = (int)m68k_get_reg(NULL, M68K_REG_D4);
    int h = (int)m68k_get_reg(NULL, M68K_REG_D5);
    uint8_t minterm = (uint8_t)m68k_get_reg(NULL, M68K_REG_D6);

    BlitSurface src, dst;
    blit_surface_from_bitmap(&src, src_bm);
    blit_surface_from_rastport(&dst, dst_rp);
    blit_rect(&src, sx, sy, &dst, dx, dy, w, h, minterm, 0, 0, 0, 0);
}

static void graphics_BltMaskBitMapRastPort(void)
{
    /* BltMaskBitMapRastPort(src, xSrc, ySrc, dstRP, xDst, yDst, xSize, ySize, minterm, mask)
     * A0 = src, D0 = xSrc, D1 = ySrc, A1 = dstRP, D2 = xDst, D3 = yDst,
     * D4 = xSize, D5 = ySize, D6 = minterm, A2 = mask
     */
    uint32_t src_bm = m68k_get_reg(NULL, M68K_REG_A0);
    int sx = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int sy = (int)m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t dst_rp = m68k_get_reg(NULL, M68K_REG_A1);
    int dx = (int)m68k_get_reg(NULL, M68K_REG_D2);
    int dy = (int)m68k_get_reg(NULL, M68K_REG_D3);
    int w = (int)m68k_get_reg(NULL, M68K_REG_D4);
    int h = (int)m68k_get_reg(NULL, M68K_REG_D5);
    uint8_t minterm = (uint8_t)m68k_get_reg(NULL, M68K_REG_D6);
    uint32_t mask = m68k_get_reg(NULL, M68K_REG_A2);

    BlitSurface src, dst;
    blit_surface_from_bitmap(&src, src_bm);
    blit_surface_from_rastport(&dst, dst_rp);
    int mask_bpr = mask ? (w + 15) / 16 * 2 : 0;
    blit_rect(&src, sx, sy, &dst, dx, dy, w, h, minterm, mask, mask_bpr, 0, 0);
}

static void scroll_raster(uint32_t rp, int dx, int dy,
                          int x1, int y1, int x2, int y2,
                          int fill_bg)
{
    if (x1 > x2 || y1 > y2) return;

    BlitSurface s;
    blit_surface_from_rastport(&s, rp);

    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    if (w <= 0 || h <= 0) return;

    uint32_t tmp_size = (uint32_t)w * (uint32_t)h * 4;
    uint32_t tmp = 0;
    dos_AllocMem_glue(tmp_size, 0, &tmp);
    if (!tmp) return;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = 0;
            blit_surface_get(&s, x1 + x, y1 + y, &p);
            m68k_write_memory_32(tmp + ((uint32_t)y * (uint32_t)w + (uint32_t)x) * 4, p);
        }
    }

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = m68k_read_memory_32(tmp + ((uint32_t)y * (uint32_t)w + (uint32_t)x) * 4);
            blit_surface_put(&s, x1 + x + dx, y1 + y + dy, p);
        }
    }

    dos_FreeMem_glue(tmp, tmp_size);

    if (!fill_bg) return;

    /* Fill the exposed area with the background pen. */
    uint32_t bg = rp ? current_bg(rp) : 0;
    int sx1 = x1, sy1 = y1, sx2 = x2, sy2 = y2;
    int dx1 = x1 + dx, dy1 = y1 + dy, dx2 = x2 + dx, dy2 = y2 + dy;

    if (dy > 0) {
        /* Top strip exposed */
        for (int y = sy1; y < dy1 && y <= sy2; y++)
            for (int x = sx1; x <= sx2; x++)
                blit_surface_put(&s, x, y, bg);
    } else if (dy < 0) {
        /* Bottom strip exposed */
        for (int y = dy2 + 1; y <= sy2; y++)
            for (int x = sx1; x <= sx2; x++)
                blit_surface_put(&s, x, y, bg);
    }

    if (dx > 0) {
        /* Left strip exposed (excluding already filled corner) */
        int top = (dy > 0) ? dy1 : sy1;
        int bottom = (dy < 0) ? dy2 : sy2;
        for (int y = top; y <= bottom; y++)
            for (int x = sx1; x < dx1 && x <= sx2; x++)
                blit_surface_put(&s, x, y, bg);
    } else if (dx < 0) {
        /* Right strip exposed (excluding already filled corner) */
        int top = (dy > 0) ? dy1 : sy1;
        int bottom = (dy < 0) ? dy2 : sy2;
        for (int y = top; y <= bottom; y++)
            for (int x = dx2 + 1; x <= sx2; x++)
                blit_surface_put(&s, x, y, bg);
    }
}

static void graphics_ScrollRaster(void)
{
    /* ScrollRaster(rp, dx, dy, xMin, yMin, xMax, yMax)
     * A1 = rp, D0 = dx, D1 = dy, D2 = xMin, D3 = yMin, D4 = xMax, D5 = yMax
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int dx = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int dy = (int)m68k_get_reg(NULL, M68K_REG_D1);
    int x1 = (int)m68k_get_reg(NULL, M68K_REG_D2);
    int y1 = (int)m68k_get_reg(NULL, M68K_REG_D3);
    int x2 = (int)m68k_get_reg(NULL, M68K_REG_D4);
    int y2 = (int)m68k_get_reg(NULL, M68K_REG_D5);
    scroll_raster(rp, dx, dy, x1, y1, x2, y2, 0);
}

static void graphics_ScrollRasterBF(void)
{
    /* ScrollRasterBF(rp, dx, dy, xMin, yMin, xMax, yMax)
     * A1 = rp, D0 = dx, D1 = dy, D2 = xMin, D3 = yMin, D4 = xMax, D5 = yMax
     * Scrolls and fills the exposed area with the background pen.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int dx = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int dy = (int)m68k_get_reg(NULL, M68K_REG_D1);
    int x1 = (int)m68k_get_reg(NULL, M68K_REG_D2);
    int y1 = (int)m68k_get_reg(NULL, M68K_REG_D3);
    int x2 = (int)m68k_get_reg(NULL, M68K_REG_D4);
    int y2 = (int)m68k_get_reg(NULL, M68K_REG_D5);
    scroll_raster(rp, dx, dy, x1, y1, x2, y2, 1);
}

static void graphics_ReadPixel(void)
{
    /* ReadPixel(rp, x, y)
     * A1 = rp, D0 = x, D1 = y
     * Returns the colour value in D0.  For planar BitMaps this is the pen
     * index; for the screen framebuffer it is the 32-bit RGB value.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int x = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int y = (int)m68k_get_reg(NULL, M68K_REG_D1);
    BlitSurface s;
    blit_surface_from_rastport(&s, rp);
    uint32_t col = 0;
    blit_surface_get(&s, x, y, &col);
    m68k_set_reg(M68K_REG_D0, col);
}

static void graphics_WritePixel(void)
{
    /* WritePixel(rp, x, y)
     * A1 = rp, D0 = x, D1 = y
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int x = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int y = (int)m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t col = current_fg(rp);
    BlitSurface s;
    blit_surface_from_rastport(&s, rp);
    blit_surface_put(&s, x, y, col);
}

static void graphics_ReadPixelLine8(void)
{
    /* ReadPixelLine8(rp, x, y, width, array)
     * A1 = rp, D0 = x, D1 = y, D2 = width, A0 = array
     * Reads a horizontal line of pixels and writes them as UBYTE values.
     * For the screen framebuffer the values are greyscale; for planar
     * BitMaps they are the pen indices.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int x = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int y = (int)m68k_get_reg(NULL, M68K_REG_D1);
    int w = (int)m68k_get_reg(NULL, M68K_REG_D2);
    uint32_t array = m68k_get_reg(NULL, M68K_REG_A0);
    if (!array || w <= 0) { m68k_set_reg(M68K_REG_D0, 0); return; }
    BlitSurface s;
    blit_surface_from_rastport(&s, rp);
    for (int i = 0; i < w; i++) {
        uint32_t v = 0;
        blit_surface_get(&s, x + i, y, &v);
        if (s.is_fb) v = rgb_to_grey8(v);
        m68k_write_memory_8(array + i, (uint8_t)v);
    }
    m68k_set_reg(M68K_REG_D0, (unsigned int)w);
}

static void graphics_WritePixelLine8(void)
{
    /* WritePixelLine8(rp, x, y, width, array, temp)
     * A1 = rp, D0 = x, D1 = y, D2 = width, A0 = array, A2 = temp
     * Writes a horizontal line of chunky UBYTE pixels to the RastPort surface.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int x = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int y = (int)m68k_get_reg(NULL, M68K_REG_D1);
    int w = (int)m68k_get_reg(NULL, M68K_REG_D2);
    uint32_t array = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t temp = m68k_get_reg(NULL, M68K_REG_A2);
    (void)temp;
    if (!array || w <= 0) { m68k_set_reg(M68K_REG_D0, 0); return; }
    BlitSurface s;
    blit_surface_from_rastport(&s, rp);
    for (int i = 0; i < w; i++) {
        uint8_t pen = (uint8_t)m68k_read_memory_8(array + i);
        blit_surface_put(&s, x + i, y, pen);
    }
    m68k_set_reg(M68K_REG_D0, (unsigned int)w);
}

static void graphics_ReadPixelArray8(void)
{
    /* ReadPixelArray8(rp, x, y, xSize, ySize, array, arrayBytesPerRow)
     * A1 = rp, D0 = x, D1 = y, D2 = xSize, D3 = ySize, A0 = array, A2 = arrayBytesPerRow
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int x = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int y = (int)m68k_get_reg(NULL, M68K_REG_D1);
    int w = (int)m68k_get_reg(NULL, M68K_REG_D2);
    int h = (int)m68k_get_reg(NULL, M68K_REG_D3);
    uint32_t array = m68k_get_reg(NULL, M68K_REG_A0);
    int bpr = (int)m68k_get_reg(NULL, M68K_REG_A2);
    if (!array || w <= 0 || h <= 0 || bpr <= 0) { m68k_set_reg(M68K_REG_D0, 0); return; }
    BlitSurface s;
    blit_surface_from_rastport(&s, rp);
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint32_t v = 0;
            blit_surface_get(&s, x + col, y + row, &v);
            if (s.is_fb) v = rgb_to_grey8(v);
            m68k_write_memory_8(array + row * bpr + col, (uint8_t)v);
        }
    }
    m68k_set_reg(M68K_REG_D0, (unsigned int)(w * h));
}

static void graphics_WritePixelArray8(void)
{
    /* WritePixelArray8(rp, x, y, xSize, ySize, array, arrayBytesPerRow)
     * A1 = rp, D0 = x, D1 = y, D2 = xSize, D3 = ySize, A0 = array, A2 = arrayBytesPerRow
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int x = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int y = (int)m68k_get_reg(NULL, M68K_REG_D1);
    int w = (int)m68k_get_reg(NULL, M68K_REG_D2);
    int h = (int)m68k_get_reg(NULL, M68K_REG_D3);
    uint32_t array = m68k_get_reg(NULL, M68K_REG_A0);
    int bpr = (int)m68k_get_reg(NULL, M68K_REG_A2);
    if (!array || w <= 0 || h <= 0 || bpr <= 0) { m68k_set_reg(M68K_REG_D0, 0); return; }
    BlitSurface s;
    blit_surface_from_rastport(&s, rp);
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint8_t pen = (uint8_t)m68k_read_memory_8(array + row * bpr + col);
            blit_surface_put(&s, x + col, y + row, pen);
        }
    }
    m68k_set_reg(M68K_REG_D0, (unsigned int)(w * h));
}

static void graphics_WriteChunkyPixels(void)
{
    /* WriteChunkyPixels(rp, x, y, xSize, ySize, array, arrayBytesPerRow)
     * A1 = rp, D0 = x, D1 = y, D2 = xSize, D3 = ySize, A0 = array, A2 = arrayBytesPerRow
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    int x = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int y = (int)m68k_get_reg(NULL, M68K_REG_D1);
    int w = (int)m68k_get_reg(NULL, M68K_REG_D2);
    int h = (int)m68k_get_reg(NULL, M68K_REG_D3);
    uint32_t array = m68k_get_reg(NULL, M68K_REG_A0);
    int bpr = (int)m68k_get_reg(NULL, M68K_REG_A2);
    if (!array || w <= 0 || h <= 0 || bpr <= 0) { m68k_set_reg(M68K_REG_D0, 0); return; }
    BlitSurface s;
    blit_surface_from_rastport(&s, rp);
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint8_t pen = (uint8_t)m68k_read_memory_8(array + row * bpr + col);
            blit_surface_put(&s, x + col, y + row, pen);
        }
    }
    m68k_set_reg(M68K_REG_D0, (unsigned int)(w * h));
}

static void graphics_AllocBitMap(void)
{
    /* AllocBitMap(width, height, depth, flags, friendBitmap)
     * D0 = width, D1 = height, D2 = depth, D3 = flags, A0 = friendBitmap
     * Returns a BitMap pointer in D0, or 0 on failure.
     *
     * UAOS BitMaps are planar Amiga-style: each plane is width/8 bytes
     * per row (rounded up to a multiple of 2), stored as depth separate
     * plane pointers in Planes[0..depth-1].
     */
    int w = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int h = (int)m68k_get_reg(NULL, M68K_REG_D1);
    int d = (int)m68k_get_reg(NULL, M68K_REG_D2);
    uint32_t flags = m68k_get_reg(NULL, M68K_REG_D3);
    uint32_t friend = m68k_get_reg(NULL, M68K_REG_A0);
    (void)friend;

    if (w <= 0 || h <= 0 || d <= 0 || d > 8) { m68k_set_reg(M68K_REG_D0, 0); return; }

    uint32_t bm = 0;
    dos_AllocMem_glue(64, 0, &bm);
    if (!bm) { m68k_set_reg(M68K_REG_D0, 0); return; }

    for (int i = 0; i < 64; i++)
        m68k_write_memory_8(bm + i, 0);

    uint16_t bpr = (uint16_t)(((w + 15) / 16) * 2);
    if (bpr < 2) bpr = 2;
    uint32_t plane_size = (uint32_t)bpr * (uint32_t)h;
    uint32_t planes[8] = {0};
    int failed = 0;

    for (int p = 0; p < d; p++) {
        uint32_t plane = 0;
        dos_AllocMem_glue(plane_size, 0, &plane);
        if (!plane) { failed = 1; break; }
        if (flags & BMF_CLEAR) {
            for (uint32_t i = 0; i < plane_size; i++)
                m68k_write_memory_8(plane + i, 0);
        }
        planes[p] = plane;
    }

    if (failed) {
        for (int p = 0; p < d; p++)
            if (planes[p]) dos_FreeMem_glue(planes[p], plane_size);
        dos_FreeMem_glue(bm, 64);
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    m68k_write_memory_16(bm + BM_OFF_BYTESPERROW, bpr);
    m68k_write_memory_16(bm + BM_OFF_ROWS,         (uint16_t)h);
    m68k_write_memory_8 (bm + BM_OFF_FLAGS,        (uint8_t)flags);
    m68k_write_memory_8 (bm + BM_OFF_DEPTH,        (uint8_t)d);
    for (int p = 0; p < 8; p++)
        m68k_write_memory_32(bm + BM_OFF_PLANES + p * 4, planes[p]);

    m68k_set_reg(M68K_REG_D0, bm);
}

static void graphics_FreeBitMap(void)
{
    /* FreeBitMap(bitmap) — A1 = bitmap
     * Free a BitMap allocated by AllocBitMap.
     */
    uint32_t bm = m68k_get_reg(NULL, M68K_REG_A1);
    if (!bm) return;

    uint8_t depth = m68k_read_memory_8(bm + BM_OFF_DEPTH);
    if (depth == 0) depth = 1;
    if (depth > 8) depth = 8;
    uint16_t bpr  = m68k_read_memory_16(bm + BM_OFF_BYTESPERROW);
    uint16_t rows = m68k_read_memory_16(bm + BM_OFF_ROWS);
    uint32_t plane_size = (uint32_t)bpr * (uint32_t)rows;

    for (int p = 0; p < depth; p++) {
        uint32_t plane = m68k_read_memory_32(bm + BM_OFF_PLANES + p * 4);
        if (plane && plane_size)
            dos_FreeMem_glue(plane, plane_size);
    }

    dos_FreeMem_glue(bm, 64);
}

static void graphics_GetBitMap(void)
{
    /* GetBitMap(rp) — A1 = rp
     * Helper that returns the RastPort's BitMap pointer in D0.
     * (Not a standard AmigaOS graphics.library LVO; provided for
     *  internal use and callers that expect a getter.)
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t bm = rp ? m68k_read_memory_32(rp + RP_OFF_BITMAP) : 0;
    m68k_set_reg(M68K_REG_D0, bm);
}

static void graphics_AllocRaster(void)
{
    /* AllocRaster(width, height)
     * D0 = width, D1 = height
     * Returns a planar raster buffer in D0, zeroed.
     */
    int w = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int h = (int)m68k_get_reg(NULL, M68K_REG_D1);
    if (w <= 0 || h <= 0) { m68k_set_reg(M68K_REG_D0, 0); return; }

    uint32_t size = (uint32_t)((((w + 15) / 16) * 2) * h);
    uint32_t addr = 0;
    dos_AllocMem_glue(size, 0, &addr);
    if (addr) {
        for (uint32_t i = 0; i < size; i++)
            m68k_write_memory_8(addr + i, 0);
    }
    m68k_set_reg(M68K_REG_D0, addr);
}

static void graphics_FreeRaster(void)
{
    /* FreeRaster(mem, width, height)
     * A0 = mem, D0 = width, D1 = height
     */
    uint32_t addr = m68k_get_reg(NULL, M68K_REG_A0);
    int w = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int h = (int)m68k_get_reg(NULL, M68K_REG_D1);
    if (!addr || w <= 0 || h <= 0) return;

    uint32_t size = (uint32_t)((((w + 15) / 16) * 2) * h);
    dos_FreeMem_glue(addr, size);
}

static void graphics_InitTmpRas(void)
{
    /* InitTmpRas(tmpRas, buffer, size)
     * A0 = tmpRas, A1 = buffer, D0 = size
     * Initialises a TmpRas structure (8 bytes: buffer, size).
     */
    uint32_t tmpRas = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t buffer = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t size   = m68k_get_reg(NULL, M68K_REG_D0);
    if (!tmpRas) return;
    m68k_write_memory_32(tmpRas,     buffer);
    m68k_write_memory_32(tmpRas + 4, size);
}

static void graphics_LoadRGB4(void)
{
    /* LoadRGB4(vp, colors, count)
     * A0 = vp, A1 = colors, D0 = count
     * colors is an array of UWORD: bits 11-8 R, 7-4 G, 3-0 B.
     */
    uint32_t vp = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t colors = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t count = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t cm = vp_get_colormap(vp);
    if (!cm || !colors) return;

    uint32_t max = cm_get_count(cm);
    for (uint32_t i = 0; i < count && i < max; i++) {
        uint16_t packed = m68k_read_memory_16(colors + i * 2);
        uint8_t r = (uint8_t)((packed >> 8) & 0x0F);
        uint8_t g = (uint8_t)((packed >> 4) & 0x0F);
        uint8_t b = (uint8_t)(packed & 0x0F);
        cm_set_color(cm, i, rgb4_to_rgb32(r, g, b));
    }
}

static void graphics_SetRGB32(void)
{
    /* SetRGB32(vp, index, r, g, b)
     * A0 = vp, D0 = index, D1 = r, D2 = g, D3 = b
     * 32-bit components are taken from the high byte.
     */
    uint32_t vp = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t index = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t r = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t g = m68k_get_reg(NULL, M68K_REG_D2);
    uint32_t b = m68k_get_reg(NULL, M68K_REG_D3);
    uint32_t cm = vp_get_colormap(vp);
    cm_set_color(cm, index, rgb32_from_32bit(r >> 24, g >> 24, b >> 24));
}

static void graphics_LoadRGB32(void)
{
    /* LoadRGB32(vp, count, table)
     * A0 = vp, D0 = count, A1 = table
     * table is an array of {r, g, b} 32-bit values.
     */
    uint32_t vp = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t count = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t table = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t cm = vp_get_colormap(vp);
    if (!cm || !table) return;

    uint32_t max = cm_get_count(cm);
    for (uint32_t i = 0; i < count && i < max; i++) {
        uint32_t r = m68k_read_memory_32(table + i * 12);
        uint32_t g = m68k_read_memory_32(table + i * 12 + 4);
        uint32_t b = m68k_read_memory_32(table + i * 12 + 8);
        cm_set_color(cm, i, rgb32_from_32bit(r >> 24, g >> 24, b >> 24));
    }
}

static void graphics_GetColorMap(void)
{
    /* GetColorMap(entries) — D0 = entries
     * Returns a ColorMap pointer in D0.
     */
    uint32_t entries = m68k_get_reg(NULL, M68K_REG_D0);
    if (entries == 0) { m68k_set_reg(M68K_REG_D0, 0); return; }

    uint32_t cm = 0;
    dos_AllocMem_glue(CM_SIZE, 0, &cm);
    if (!cm) { m68k_set_reg(M68K_REG_D0, 0); return; }

    uint32_t table_size = entries * 4;
    uint32_t table = 0;
    dos_AllocMem_glue(table_size, 0, &table);
    if (!table) {
        dos_FreeMem_glue(cm, CM_SIZE);
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    for (int i = 0; i < CM_SIZE; i++) m68k_write_memory_8(cm + i, 0);
    for (uint32_t i = 0; i < table_size / 4; i++) m68k_write_memory_32(table + i * 4, 0);

    m68k_write_memory_8(cm + CM_OFF_TYPE, 1);
    m68k_write_memory_16(cm + CM_OFF_COUNT, (uint16_t)entries);
    m68k_write_memory_16(cm + CM_OFF_TABLEENTRIES, (uint16_t)entries);
    m68k_write_memory_32(cm + CM_OFF_COLORTABLE, table);
    m68k_write_memory_32(cm + CM_OFF_PALEXTRA, 0);

    m68k_set_reg(M68K_REG_D0, cm);
}

static uint32_t cm_get_palextra(uint32_t cm);
static uint32_t pal_extra_size(uint32_t count);

static void graphics_FreeColorMap(void)
{
    /* FreeColorMap(cm) — A0 = cm */
    uint32_t cm = m68k_get_reg(NULL, M68K_REG_A0);
    if (!cm) return;

    uint32_t table = cm_get_colortable(cm);
    uint32_t pe = cm_get_palextra(cm);
    uint32_t count = cm_get_count(cm);
    if (table && count) dos_FreeMem_glue(table, count * 4);
    if (pe) {
        uint32_t pe_size = pal_extra_size(count);
        dos_FreeMem_glue(pe, pe_size);
    }
    dos_FreeMem_glue(cm, CM_SIZE);
}

static void graphics_GetRGB4(void)
{
    /* GetRGB4(vp, index) — A0 = vp, D0 = index
     * Returns packed RGB4 in D0.
     */
    uint32_t vp = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t index = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t cm = vp_get_colormap(vp);
    uint32_t rgb = cm_get_color(cm, index);
    uint8_t r, g, b;
    rgb32_to_rgb4(rgb, &r, &g, &b);
    m68k_set_reg(M68K_REG_D0, ((uint32_t)r << 8) | ((uint32_t)g << 4) | (uint32_t)b);
}

static void graphics_GetRGB32(void)
{
    /* GetRGB32(cm, first, count, table)
     * A0 = cm, D0 = first, D1 = count, A1 = table
     */
    uint32_t cm = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t first = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t count = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t table = m68k_get_reg(NULL, M68K_REG_A1);
    if (!cm || !table) return;

    uint32_t max = cm_get_count(cm);
    for (uint32_t i = 0; i < count && (first + i) < max; i++) {
        uint32_t rgb = cm_get_color(cm, first + i);
        m68k_write_memory_32(table + i * 4, rgb);
    }
}

static void graphics_SetRGB4CM(void)
{
    /* SetRGB4CM(cm, index, r, g, b) — A0 = cm, D0 = index, D1 = r, D2 = g, D3 = b */
    uint32_t cm = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t index = m68k_get_reg(NULL, M68K_REG_D0);
    uint8_t r = (uint8_t)m68k_get_reg(NULL, M68K_REG_D1);
    uint8_t g = (uint8_t)m68k_get_reg(NULL, M68K_REG_D2);
    uint8_t b = (uint8_t)m68k_get_reg(NULL, M68K_REG_D3);
    cm_set_color(cm, index, rgb4_to_rgb32(r, g, b));
}

static void graphics_SetRGB32CM(void)
{
    /* SetRGB32CM(cm, index, r, g, b) — A0 = cm, D0 = index, D1 = r, D2 = g, D3 = b */
    uint32_t cm = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t index = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t r = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t g = m68k_get_reg(NULL, M68K_REG_D2);
    uint32_t b = m68k_get_reg(NULL, M68K_REG_D3);
    cm_set_color(cm, index, rgb32_from_32bit(r >> 24, g >> 24, b >> 24));
}

static void graphics_FindColor(void)
{
    /* FindColor(cm, r, g, b, tolerance)
     * A0 = cm, D1 = r, D2 = g, D3 = b, D4 = tolerance
     * Returns pen index in D0, or -1 if none within tolerance.
     */
    uint32_t cm = m68k_get_reg(NULL, M68K_REG_A0);
    uint8_t r = (uint8_t)m68k_get_reg(NULL, M68K_REG_D1);
    uint8_t g = (uint8_t)m68k_get_reg(NULL, M68K_REG_D2);
    uint8_t b = (uint8_t)m68k_get_reg(NULL, M68K_REG_D3);
    uint32_t tolerance = m68k_get_reg(NULL, M68K_REG_D4);
    uint32_t target = rgb4_to_rgb32(r, g, b);

    if (!cm) { m68k_set_reg(M68K_REG_D0, 0xFFFFFFFF); return; }

    uint32_t count = cm_get_count(cm);
    int best_pen = -1;
    uint32_t best_dist = 0xFFFFFFFF;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t rgb = cm_get_color(cm, i);
        int dr = (int)((rgb >> 16) & 0xFF) - (int)((target >> 16) & 0xFF);
        int dg = (int)((rgb >> 8) & 0xFF) - (int)((target >> 8) & 0xFF);
        int db = (int)(rgb & 0xFF) - (int)(target & 0xFF);
        uint32_t dist = (uint32_t)(dr * dr + dg * dg + db * db);
        if (dist <= tolerance && dist < best_dist) {
            best_dist = dist;
            best_pen = (int)i;
        }
    }
    m68k_set_reg(M68K_REG_D0, (best_pen < 0) ? 0xFFFFFFFF : (uint32_t)best_pen);
}

static uint32_t cm_get_palextra(uint32_t cm)
{
    if (!cm) return 0;
    return m68k_read_memory_32(cm + CM_OFF_PALEXTRA);
}

static void cm_set_palextra(uint32_t cm, uint32_t pe)
{
    if (!cm) return;
    m68k_write_memory_32(cm + CM_OFF_PALEXTRA, pe);
}

static uint32_t pal_extra_size(uint32_t count)
{
    return count + (count + 7) / 8;
}

static uint8_t pe_get_ref(uint32_t pe, uint32_t pen)
{
    if (!pe) return 0;
    return m68k_read_memory_8(pe + pen);
}

static void pe_set_ref(uint32_t pe, uint32_t pen, uint8_t ref)
{
    if (!pe) return;
    m68k_write_memory_8(pe + pen, ref);
}

static int pe_is_alloc(uint32_t pe, uint32_t pen)
{
    if (!pe) return 0;
    uint32_t byte = pe + pen / 8;
    uint8_t bit = 7 - (pen % 8);
    uint8_t b = m68k_read_memory_8(byte);
    return (b & (1 << bit)) ? 1 : 0;
}

static void pe_set_alloc(uint32_t pe, uint32_t pen, int alloc)
{
    if (!pe) return;
    uint32_t byte = pe + pen / 8;
    uint8_t bit = 7 - (pen % 8);
    uint8_t b = m68k_read_memory_8(byte);
    if (alloc) b |= (1 << bit);
    else b &= ~(1 << bit);
    m68k_write_memory_8(byte, b);
}

static void graphics_AttachPalExtra(void)
{
    /* AttachPalExtra(cm, vp) — A0 = cm, A1 = vp
     * Allocates and attaches a PalExtra structure to the ColorMap.
     */
    uint32_t cm = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t vp = m68k_get_reg(NULL, M68K_REG_A1);
    if (!cm) { m68k_set_reg(M68K_REG_D0, 0); return; }

    uint32_t pe = cm_get_palextra(cm);
    if (!pe) {
        uint32_t count = cm_get_count(cm);
        uint32_t size = pal_extra_size(count);
        dos_AllocMem_glue(size, 0, &pe);
        if (!pe) { m68k_set_reg(M68K_REG_D0, 0); return; }
        for (uint32_t i = 0; i < size; i++) m68k_write_memory_8(pe + i, 0);
        cm_set_palextra(cm, pe);
    }

    if (vp) m68k_write_memory_32(vp + VP_OFF_COLORMAP, cm);
    m68k_set_reg(M68K_REG_D0, 1);
}

static void graphics_ObtainPen(void)
{
    /* ObtainPen(cm, r, g, b, flags)
     * A0 = cm, D1 = r, D2 = g, D3 = b, D4 = flags
     * Returns pen index in D0, or -1 on failure.
     */
    uint32_t cm = m68k_get_reg(NULL, M68K_REG_A0);
    uint8_t r = (uint8_t)m68k_get_reg(NULL, M68K_REG_D1);
    uint8_t g = (uint8_t)m68k_get_reg(NULL, M68K_REG_D2);
    uint8_t b = (uint8_t)m68k_get_reg(NULL, M68K_REG_D3);
    uint32_t flags = m68k_get_reg(NULL, M68K_REG_D4);
    (void)flags;

    if (!cm) { m68k_set_reg(M68K_REG_D0, 0xFFFFFFFF); return; }

    uint32_t target = rgb4_to_rgb32(r, g, b);
    uint32_t count = cm_get_count(cm);
    uint32_t pe = cm_get_palextra(cm);

    for (uint32_t i = 0; i < count; i++) {
        if (cm_get_color(cm, i) == target) {
            if (pe) {
                pe_set_ref(pe, i, pe_get_ref(pe, i) + 1);
                pe_set_alloc(pe, i, 1);
            }
            m68k_set_reg(M68K_REG_D0, i);
            return;
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        if (!pe || !pe_is_alloc(pe, i)) {
            cm_set_color(cm, i, target);
            if (pe) {
                pe_set_ref(pe, i, 1);
                pe_set_alloc(pe, i, 1);
            }
            m68k_set_reg(M68K_REG_D0, i);
            return;
        }
    }

    m68k_set_reg(M68K_REG_D0, 0xFFFFFFFF);
}

static void graphics_ReleasePen(void)
{
    /* ReleasePen(cm, pen) — A0 = cm, D0 = pen */
    uint32_t cm = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t pen = m68k_get_reg(NULL, M68K_REG_D0);
    if (!cm || pen >= cm_get_count(cm)) return;

    uint32_t pe = cm_get_palextra(cm);
    if (!pe) return;

    uint8_t ref = pe_get_ref(pe, pen);
    if (ref > 0) {
        pe_set_ref(pe, pen, ref - 1);
        if (ref == 1) pe_set_alloc(pe, pen, 0);
    }
}

static void graphics_ObtainBestPenA(void)
{
    /* ObtainBestPenA(cm, r, g, b, tags)
     * A0 = cm, D1 = r, D2 = g, D3 = b, A1 = tags
     * Returns best pen index in D0, or -1.
     */
    uint32_t cm = m68k_get_reg(NULL, M68K_REG_A0);
    uint8_t r = (uint8_t)m68k_get_reg(NULL, M68K_REG_D1);
    uint8_t g = (uint8_t)m68k_get_reg(NULL, M68K_REG_D2);
    uint8_t b = (uint8_t)m68k_get_reg(NULL, M68K_REG_D3);
    (void)m68k_get_reg(NULL, M68K_REG_A1);

    if (!cm) { m68k_set_reg(M68K_REG_D0, 0xFFFFFFFF); return; }

    uint32_t target = rgb4_to_rgb32(r, g, b);
    uint32_t count = cm_get_count(cm);
    int best_pen = -1;
    uint32_t best_dist = 0xFFFFFFFF;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t rgb = cm_get_color(cm, i);
        int dr = (int)((rgb >> 16) & 0xFF) - (int)((target >> 16) & 0xFF);
        int dg = (int)((rgb >> 8) & 0xFF) - (int)((target >> 8) & 0xFF);
        int db = (int)(rgb & 0xFF) - (int)(target & 0xFF);
        uint32_t dist = (uint32_t)(dr * dr + dg * dg + db * db);
        if (dist < best_dist) {
            best_dist = dist;
            best_pen = (int)i;
        }
    }

    m68k_set_reg(M68K_REG_D0, (best_pen < 0) ? 0xFFFFFFFF : (uint32_t)best_pen);
}

static void graphics_GetAPen(void)
{
    /* GetAPen(rp) — A1 = rp; returns pen in D0 */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t pen = (rp) ? rp_u8(rp, RP_OFF_FGPEN) : 1;
    m68k_set_reg(M68K_REG_D0, pen);
}

static void graphics_GetBPen(void)
{
    /* GetBPen(rp) — A1 = rp; returns pen in D0 */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t pen = (rp) ? rp_u8(rp, RP_OFF_BGPEN) : 0;
    m68k_set_reg(M68K_REG_D0, pen);
}

static void graphics_GetDrMd(void)
{
    /* GetDrMd(rp) — A1 = rp; returns draw mode in D0 */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t mode = (rp) ? rp_u8(rp, RP_OFF_DRAWMODE) : JAM2;
    m68k_set_reg(M68K_REG_D0, mode);
}

static void graphics_GetOutlinePen(void)
{
    /* GetOutlinePen(rp) — A1 = rp; returns outline pen in D0 */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t pen = (rp) ? rp_u8(rp, RP_OFF_OLNPEN) : 0;
    m68k_set_reg(M68K_REG_D0, pen);
}

static void graphics_SetOutlinePen(void)
{
    /* SetOutlinePen(rp, pen) — A1 = rp, D0 = pen; returns old pen in D0 */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint8_t pen = (uint8_t)m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t old = 0;
    if (rp) {
        old = rp_u8(rp, RP_OFF_OLNPEN);
        rp_w_u8(rp, RP_OFF_OLNPEN, pen);
    }
    m68k_set_reg(M68K_REG_D0, old);
}

static void graphics_SetMaxPen(void)
{
    /* SetMaxPen(rp, maxpen) — A1 = rp, D0 = maxpen
     * Store the max pen in the RastPort; drawing is unaffected on our
     * truecolour framebuffer.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint8_t maxpen = (uint8_t)m68k_get_reg(NULL, M68K_REG_D0);
    if (rp) rp_w_u8(rp, RP_OFF_MAXPEN, maxpen);
}

static void graphics_SetFont(void)
{
    /* SetFont(rp, font) — A1 = rp, A0 = font
     * Returns the previous font pointer in D0.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t font = m68k_get_reg(NULL, M68K_REG_A0);
    if (!rp) { m68k_set_reg(M68K_REG_D0, 0); return; }
    uint32_t old = m68k_read_memory_32(rp + RP_OFF_FONT);
    m68k_write_memory_32(rp + RP_OFF_FONT, font);
    m68k_set_reg(M68K_REG_D0, old);
}

static void graphics_AskFont(void)
{
    /* AskFont(rp) — A1 = rp
     * Returns the current font pointer in D0.
     */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t font = rp ? m68k_read_memory_32(rp + RP_OFF_FONT) : 0;
    m68k_set_reg(M68K_REG_D0, font);
}

static void graphics_GetVPModeID(void)
{
    /* GetVPModeID(vp) — A0 = vp
     * Returns the ViewPort DisplayID in D0.
     */
    uint32_t vp = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t modeid = vp ? m68k_read_memory_32(vp + VP_OFF_DISPLAYID) : 0;
    m68k_set_reg(M68K_REG_D0, modeid);
}

static void graphics_GetBitMapAttr(void)
{
    /* GetBitMapAttr(bm, attr) — A0 = bm, D1 = attr
     * Returns the requested BitMap attribute in D0.
     * UAOS BitMaps are planar Amiga-style: width = BytesPerRow * 8.
     */
    uint32_t bm = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t attr = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t val = 0;
    if (bm) {
        uint16_t bpr = m68k_read_memory_16(bm + BM_OFF_BYTESPERROW);
        uint8_t depth = m68k_read_memory_8(bm + BM_OFF_DEPTH);
        switch (attr) {
            case BMA_WIDTH:
                val = (uint32_t)bpr * 8;
                break;
            case BMA_HEIGHT:
                val = (uint32_t)m68k_read_memory_16(bm + BM_OFF_ROWS);
                break;
            case BMA_DEPTH:
                val = (uint32_t)depth;
                break;
            case BMA_FLAGS:
                val = (uint32_t)m68k_read_memory_8(bm + BM_OFF_FLAGS);
                break;
            case BMA_BASE:
                val = m68k_read_memory_32(bm + BM_OFF_PLANES);
                break;
            case BMA_ROWBYTES:
                val = (uint32_t)bpr;
                break;
            default:
                val = 0;
                break;
        }
    }
    m68k_set_reg(M68K_REG_D0, val);
}

static void graphics_SetABPenDrMd(void)
{
    /* SetABPenDrMd(rp, apen, bpen, mode) — A1 = rp, D0 = apen, D1 = bpen, D2 = mode */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    if (rp) {
        rp_w_u8(rp, RP_OFF_FGPEN, (uint8_t)m68k_get_reg(NULL, M68K_REG_D0));
        rp_w_u8(rp, RP_OFF_BGPEN, (uint8_t)m68k_get_reg(NULL, M68K_REG_D1));
        rp_w_u8(rp, RP_OFF_DRAWMODE, (uint8_t)m68k_get_reg(NULL, M68K_REG_D2));
    }
}

static void graphics_SetRGB4(void)
{
    /* SetRGB4(vp, index, red, green, blue)
     * A0 = vp, D0 = index, D1 = red, D2 = green, D3 = blue
     * Stores the 4-bit RGB value in the ViewPort's ColorMap.
     */
    uint32_t vp = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t index = m68k_get_reg(NULL, M68K_REG_D0);
    uint8_t r = (uint8_t)m68k_get_reg(NULL, M68K_REG_D1);
    uint8_t g = (uint8_t)m68k_get_reg(NULL, M68K_REG_D2);
    uint8_t b = (uint8_t)m68k_get_reg(NULL, M68K_REG_D3);
    uint32_t cm = vp_get_colormap(vp);
    cm_set_color(cm, index, rgb4_to_rgb32(r, g, b));
}

/* =========================================================================
 * Display-mode database (static lookup tables)
 * ========================================================================= */

#define DTAG_DISP_DIM  0x80000001
#define DTAG_NAME      0x80000002
#define DTAG_MNTR      0x80000003
#define DTAG_VEC       0x80000004

#define DIM_MAXWIDTH   0
#define DIM_MAXHEIGHT  2
#define DIM_MAXDEPTH   4
#define DIM_STDIMGWIDTH  6
#define DIM_STDIMGHEIGHT 8
#define DIM_SIZE       10

#define MONITOR_NAME    0
#define MONITOR_SIZE    32

typedef struct {
    uint32_t mode_id;
    uint16_t width;
    uint16_t height;
    uint8_t  depth;
    const char *name;
} UaosDisplayMode;

/* Mode ID layout (AmigaOS 3.x compatible):
 *   bit 31: AGA flag
 *   bits 28-30: reserved
 *   bits 16-27: monitor/mode index (upper nibble = monitor, lower = mode)
 *   bits 12-15: reserved
 *   bits  8-11: depth flags (HAM/EHB/24-bit)
 *   bits  0-7:  depth in bitplanes
 */

static const UaosDisplayMode uaos_modes[] = {
    /* OCS/ECS modes */
    { 0x00011000,  320,  256, 5, "PAL:320x256" },
    { 0x00011001,  320,  200, 5, "NTSC:320x200" },
    { 0x00011002,  640,  256, 4, "DBLPAL:640x256" },
    { 0x00011003,  640,  400, 4, "DBLNTSC:640x400" },
    { 0x00011004,  640,  512, 4, "SUPER72:640x512" },
    { 0x00011005,  800,  600, 4, "EURO72:800x600" },
    { 0x00011006, 1024,  768, 4, "EURO36:1024x768" },
    { 0x00011007, 1280,  720, 4, "VESA:1280x720" },
    /* AGA modes (same geometry, up to 8 bitplanes / 256 colours / 24-bit palette) */
    { 0x80011000,  320,  256, 8, "AGA:PAL:320x256" },
    { 0x80011001,  320,  200, 8, "AGA:NTSC:320x200" },
    { 0x80011002,  640,  256, 8, "AGA:DBLPAL:640x256" },
    { 0x80011003,  640,  400, 8, "AGA:DBLNTSC:640x400" },
    { 0x80011004,  640,  512, 8, "AGA:SUPER72:640x512" },
    { 0x80011005,  800,  600, 8, "AGA:EURO72:800x600" },
    { 0x80011006, 1024,  768, 8, "AGA:EURO36:1024x768" },
    { 0x80011007, 1280,  720, 8, "AGA:VESA:1280x720" },
};
#define UAOS_MODE_COUNT (sizeof(uaos_modes) / sizeof(uaos_modes[0]))

static uint32_t g_disp_table = 0;
static uint32_t g_monitor = 0;

static void disp_init_table(void)
{
    if (g_disp_table) return;

    uint32_t names_size = 0;
    for (size_t i = 0; i < UAOS_MODE_COUNT; i++)
        names_size += (uint32_t)strlen(uaos_modes[i].name) + 1;

    uint32_t table_size = (uint32_t)(UAOS_MODE_COUNT * DI_SIZE) + names_size;
    uint32_t table = 0;
    dos_AllocMem_glue(table_size, 0, &table);
    if (!table) return;

    for (uint32_t i = 0; i < table_size; i++) m68k_write_memory_8(table + i, 0);

    uint32_t name_ptr = table + (uint32_t)(UAOS_MODE_COUNT * DI_SIZE);
    for (size_t i = 0; i < UAOS_MODE_COUNT; i++) {
        uint32_t entry = table + (uint32_t)(i * DI_SIZE);
        m68k_write_memory_32(entry + DI_MODEID, uaos_modes[i].mode_id);
        m68k_write_memory_16(entry + DI_WIDTH, uaos_modes[i].width);
        m68k_write_memory_16(entry + DI_HEIGHT, uaos_modes[i].height);
        m68k_write_memory_8(entry + DI_DEPTH, uaos_modes[i].depth);
        uint8_t flags = 0;
        if (uaos_modes[i].mode_id & AGA_FLAG) flags |= 0x01;
        m68k_write_memory_8(entry + DI_FLAGS, flags);
        m68k_write_memory_32(entry + DI_NAME, name_ptr);
        for (size_t j = 0; j <= strlen(uaos_modes[i].name); j++)
            m68k_write_memory_8(name_ptr + j, (uint8_t)uaos_modes[i].name[j]);
        name_ptr += (uint32_t)strlen(uaos_modes[i].name) + 1;
    }

    g_disp_table = table;
}

static void disp_init_monitor(void)
{
    if (g_monitor) return;
    uint32_t mon = 0;
    dos_AllocMem_glue(MONITOR_SIZE, 0, &mon);
    if (!mon) return;
    for (int i = 0; i < MONITOR_SIZE; i++) m68k_write_memory_8(mon + i, 0);
    m68k_write_memory_32(mon + MONITOR_NAME, 0);
    g_monitor = mon;
}

static uint32_t disp_find_mode(uint32_t mode_id)
{
    disp_init_table();
    if (!g_disp_table) return 0;
    for (size_t i = 0; i < UAOS_MODE_COUNT; i++) {
        if (m68k_read_memory_32(g_disp_table + (uint32_t)(i * DI_SIZE) + DI_MODEID) == mode_id)
            return g_disp_table + (uint32_t)(i * DI_SIZE);
    }
    return 0;
}

static uint32_t disp_next_mode(uint32_t mode_id)
{
    disp_init_table();
    if (!g_disp_table) return 0;
    int found = 0;
    for (size_t i = 0; i < UAOS_MODE_COUNT; i++) {
        uint32_t id = m68k_read_memory_32(g_disp_table + (uint32_t)(i * DI_SIZE) + DI_MODEID);
        if (found) return id;
        if (id == mode_id) found = 1;
    }
    if (!found && UAOS_MODE_COUNT > 0)
        return m68k_read_memory_32(g_disp_table + DI_MODEID);
    return 0;
}

static void graphics_OpenMonitor(void)
{
    /* OpenMonitor(name, ver) — A1 = name, D0 = version
     * Returns a MonitorSpec pointer in D0.
     */
    (void)m68k_get_reg(NULL, M68K_REG_A1);
    (void)m68k_get_reg(NULL, M68K_REG_D0);
    disp_init_monitor();
    m68k_set_reg(M68K_REG_D0, g_monitor);
}

static void graphics_CloseMonitor(void)
{
    /* CloseMonitor(monitorSpec) — A0 = monitorSpec */
    (void)m68k_get_reg(NULL, M68K_REG_A0);
}

static void graphics_FindDisplayInfo(void)
{
    /* FindDisplayInfo(modeID) — D0 = modeID
     * Returns a DisplayInfo pointer in D0.
     */
    uint32_t mode_id = m68k_get_reg(NULL, M68K_REG_D0);
    m68k_set_reg(M68K_REG_D0, disp_find_mode(mode_id));
}

static void graphics_NextDisplayInfo(void)
{
    /* NextDisplayInfo(modeID) — D0 = modeID
     * Returns the next mode ID in D0.
     */
    uint32_t mode_id = m68k_get_reg(NULL, M68K_REG_D0);
    m68k_set_reg(M68K_REG_D0, disp_next_mode(mode_id));
}

static void graphics_GetDisplayInfoData(void)
{
    /* GetDisplayInfoData(displayInfo, buf, size, tagID, monitorSpec)
     * A0 = displayInfo, A1 = buf, D0 = size, D1 = tagID, A2 = monitorSpec
     */
    uint32_t di = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t buf = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t size = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t tag = m68k_get_reg(NULL, M68K_REG_D1);
    (void)m68k_get_reg(NULL, M68K_REG_A2);
    if (!di || !buf || size == 0) { m68k_set_reg(M68K_REG_D0, 0); return; }

    uint32_t written = 0;
    switch (tag) {
        case DTAG_DISP_DIM: {
            if (size < DIM_SIZE) { m68k_set_reg(M68K_REG_D0, 0); return; }
            uint16_t w = m68k_read_memory_16(di + DI_WIDTH);
            uint16_t h = m68k_read_memory_16(di + DI_HEIGHT);
            uint8_t d = m68k_read_memory_8(di + DI_DEPTH);
            m68k_write_memory_16(buf + DIM_MAXWIDTH, w);
            m68k_write_memory_16(buf + DIM_MAXHEIGHT, h);
            m68k_write_memory_16(buf + DIM_MAXDEPTH, (uint16_t)d);
            m68k_write_memory_16(buf + DIM_STDIMGWIDTH, w);
            m68k_write_memory_16(buf + DIM_STDIMGHEIGHT, h);
            written = DIM_SIZE;
            break;
        }
        case DTAG_NAME: {
            uint32_t name = m68k_read_memory_32(di + DI_NAME);
            if (!name) { m68k_set_reg(M68K_REG_D0, 0); return; }
            uint32_t i = 0;
            while (i < size - 1) {
                uint8_t c = m68k_read_memory_8(name + i);
                m68k_write_memory_8(buf + i, c);
                if (c == 0) break;
                i++;
            }
            m68k_write_memory_8(buf + i, 0);
            written = i + 1;
            break;
        }
        default:
            break;
    }
    m68k_set_reg(M68K_REG_D0, written);
}

static void graphics_ModeNotAvailable(void)
{
    /* ModeNotAvailable(modeID) — D0 = modeID
     * Returns 0 if available, non-zero if not.
     */
    uint32_t mode_id = m68k_get_reg(NULL, M68K_REG_D0);
    m68k_set_reg(M68K_REG_D0, disp_find_mode(mode_id) ? 0 : 1);
}

static void graphics_BestModeIDA(void)
{
    /* BestModeIDA(tags) — A0 = tags
     * Returns a suitable mode ID in D0.
     */
    (void)m68k_get_reg(NULL, M68K_REG_A0);
    disp_init_table();
    uint32_t id = 0;
    if (UAOS_MODE_COUNT > 0 && g_disp_table)
        id = m68k_read_memory_32(g_disp_table + DI_MODEID);
    m68k_set_reg(M68K_REG_D0, id);
}

static void graphics_CoerceMode(void)
{
    /* CoerceMode(modeID, flags, monitorSpec)
     * D0 = modeID, D1 = flags, A0 = monitorSpec
     * Returns coerced mode ID in D0.
     */
    uint32_t mode_id = m68k_get_reg(NULL, M68K_REG_D0);
    (void)m68k_get_reg(NULL, M68K_REG_D1);
    (void)m68k_get_reg(NULL, M68K_REG_A0);
    if (disp_find_mode(mode_id)) {
        m68k_set_reg(M68K_REG_D0, mode_id);
    } else {
        disp_init_table();
        uint32_t id = 0;
        if (UAOS_MODE_COUNT > 0 && g_disp_table)
            id = m68k_read_memory_32(g_disp_table + DI_MODEID);
        m68k_set_reg(M68K_REG_D0, id);
    }
}

static void graphics_VideoControl(void)
{
    /* VideoControl(colorMap, tags) — A0 = colorMap, A1 = tags
     * Parse the tag list and update the ColorMap/viewport extension
     * attributes stored in the ColorMap. Returns 0 in D0 on success.
     */
    uint32_t cm = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t tags = m68k_get_reg(NULL, M68K_REG_A1);

    if (!cm || !tags) {
        m68k_set_reg(M68K_REG_D0, 1);
        return;
    }

    for (uint32_t p = tags; ; p += 8) {
        uint32_t tag = m68k_read_memory_32(p);
        uint32_t data = m68k_read_memory_32(p + 4);
        if (tag == 0) break; /* TAG_DONE / VTAG_END_CM */

        switch (tag) {
            case VTAG_ATTACH_CM_SET:
                m68k_write_memory_32(cm + CM_OFF_VP, data);
                break;
            case VTAG_ATTACH_CM_GET:
                m68k_write_memory_32(data, m68k_read_memory_32(cm + CM_OFF_VP));
                break;
            case VTAG_VIEWPORTEXTRA_SET:
                m68k_write_memory_32(cm + CM_OFF_VPE, data);
                break;
            case VTAG_VIEWPORTEXTRA_GET:
                m68k_write_memory_32(data, m68k_read_memory_32(cm + CM_OFF_VPE));
                break;
            case VTAG_NORMAL_DISP_SET:
                m68k_write_memory_32(cm + CM_OFF_NORMAL_DISP, data);
                break;
            case VTAG_NORMAL_DISP_GET:
                m68k_write_memory_32(data, m68k_read_memory_32(cm + CM_OFF_NORMAL_DISP));
                break;
            case VTAG_COERCE_DISP_SET:
                m68k_write_memory_32(cm + CM_OFF_COERCE_DISP, data);
                break;
            case VTAG_COERCE_DISP_GET:
                m68k_write_memory_32(data, m68k_read_memory_32(cm + CM_OFF_COERCE_DISP));
                break;
            case VTAG_VPMODEID_SET:
                m68k_write_memory_32(cm + CM_OFF_VPMODEID, data);
                break;
            case VTAG_VPMODEID_GET:
                m68k_write_memory_32(data, m68k_read_memory_32(cm + CM_OFF_VPMODEID));
                break;
            case VTAG_VPMODEID_CLR:
                m68k_write_memory_32(cm + CM_OFF_VPMODEID, 0);
                break;
            case VTAG_BORDERBLANK_SET:
                m68k_write_memory_8(cm + CM_OFF_BORDERBLANK, 1);
                break;
            case VTAG_BORDERBLANK_CLR:
                m68k_write_memory_8(cm + CM_OFF_BORDERBLANK, 0);
                break;
            case VTAG_BORDERBLANK_GET:
                m68k_write_memory_32(data, m68k_read_memory_8(cm + CM_OFF_BORDERBLANK));
                break;
            case VTAG_CHROMAKEY_SET:
                m68k_write_memory_8(cm + CM_OFF_CHROMAKEY, 1);
                break;
            case VTAG_CHROMAKEY_CLR:
                m68k_write_memory_8(cm + CM_OFF_CHROMAKEY, 0);
                break;
            case VTAG_CHROMAKEY_GET:
                m68k_write_memory_32(data, m68k_read_memory_8(cm + CM_OFF_CHROMAKEY));
                break;
            case VTAG_BITPLANEKEY_SET:
                m68k_write_memory_8(cm + CM_OFF_BITPLANEKEY, 1);
                break;
            case VTAG_BITPLANEKEY_CLR:
                m68k_write_memory_8(cm + CM_OFF_BITPLANEKEY, 0);
                break;
            case VTAG_BITPLANEKEY_GET:
                m68k_write_memory_32(data, m68k_read_memory_8(cm + CM_OFF_BITPLANEKEY));
                break;
            case VTAG_CHROMA_PEN_SET:
                m68k_write_memory_8(cm + CM_OFF_CHROMA_PEN, (uint8_t)data);
                break;
            case VTAG_CHROMA_PEN_CLR:
                m68k_write_memory_8(cm + CM_OFF_CHROMA_PEN, 0);
                break;
            case VTAG_CHROMA_PEN_GET:
                m68k_write_memory_32(data, m68k_read_memory_8(cm + CM_OFF_CHROMA_PEN));
                break;
            case VTAG_USERCLIP_SET:
                m68k_write_memory_8(cm + CM_OFF_USERCLIP, 1);
                break;
            case VTAG_USERCLIP_CLR:
                m68k_write_memory_8(cm + CM_OFF_USERCLIP, 0);
                break;
            case VTAG_USERCLIP_GET:
                m68k_write_memory_32(data, m68k_read_memory_8(cm + CM_OFF_USERCLIP));
                break;
            case VTAG_PF1_BASE_SET:
                m68k_write_memory_8(cm + CM_OFF_PF1_BASE, (uint8_t)data);
                break;
            case VTAG_PF1_BASE_GET:
                m68k_write_memory_32(data, m68k_read_memory_8(cm + CM_OFF_PF1_BASE));
                break;
            case VTAG_PF2_BASE_SET:
                m68k_write_memory_8(cm + CM_OFF_PF2_BASE, (uint8_t)data);
                break;
            case VTAG_PF2_BASE_GET:
                m68k_write_memory_32(data, m68k_read_memory_8(cm + CM_OFF_PF2_BASE));
                break;
            case VTAG_SPEVEN_BASE_SET:
                m68k_write_memory_8(cm + CM_OFF_SPEVEN_BASE, (uint8_t)data);
                break;
            case VTAG_SPEVEN_BASE_GET:
                m68k_write_memory_32(data, m68k_read_memory_8(cm + CM_OFF_SPEVEN_BASE));
                break;
            case VTAG_SPODD_BASE_SET:
                m68k_write_memory_8(cm + CM_OFF_SPODD_BASE, (uint8_t)data);
                break;
            case VTAG_SPODD_BASE_GET:
                m68k_write_memory_32(data, m68k_read_memory_8(cm + CM_OFF_SPODD_BASE));
                break;
            /* V39+ batch and sprite tags are accepted but not acted upon */
            case VTAG_NEXTBUF_CM:
            case VTAG_BATCH_CM_SET:
            case VTAG_BATCH_CM_CLR:
            case VTAG_BATCH_ITEMS_SET:
            case VTAG_BATCH_ITEMS_GET:
            case VTAG_BATCH_ITEMS_ADD:
            case VTAG_BORDERNOTRANS_SET:
            case VTAG_BORDERNOTRANS_CLR:
            case VTAG_BORDERNOTRANS_GET:
            case VTAG_BORDERSPRITE_SET:
            case VTAG_BORDERSPRITE_CLR:
            case VTAG_BORDERSPRITE_GET:
            case VTAG_SPRITERESN_SET:
            case VTAG_SPRITERESN_GET:
            case VTAG_PF1_TO_SPRITEPRI_SET:
            case VTAG_PF1_TO_SPRITEPRI_GET:
            case VTAG_PF2_TO_SPRITEPRI_SET:
            case VTAG_PF2_TO_SPRITEPRI_GET:
            case VTAG_CHROMA_PLANE_SET:
            case VTAG_CHROMA_PLANE_GET:
            default:
                break;
        }
    }

    m68k_set_reg(M68K_REG_D0, 0);
}

static void graphics_LockLayerRom(void)
{
    /* LockLayerRom(layer) — A0 = layer
     * UAOS has no real layer locking; return success.
     */
    (void)m68k_get_reg(NULL, M68K_REG_A0);
    m68k_set_reg(M68K_REG_D0, 1);
}

static void graphics_UnlockLayerRom(void)
{
    /* UnlockLayerRom(layer) — A0 = layer */
    (void)m68k_get_reg(NULL, M68K_REG_A0);
}

static void graphics_AttemptLockLayerRom(void)
{
    /* AttemptLockLayerRom(layer) — A0 = layer
     * Always succeeds on UAOS.
     */
    (void)m68k_get_reg(NULL, M68K_REG_A0);
    m68k_set_reg(M68K_REG_D0, 1);
}

static void graphics_SyncSBitMap(void)
{
    /* SyncSBitMap(layer) — A0 = layer
     * No-op: no super-bitmap state to sync.
     */
    (void)m68k_get_reg(NULL, M68K_REG_A0);
}

static void graphics_CopySBitMap(void)
{
    /* CopySBitMap(layer) — A0 = layer
     * No-op: no super-bitmap state to copy.
     */
    (void)m68k_get_reg(NULL, M68K_REG_A0);
}

static void graphics_WaitBlit(void)
{
    /* WaitBlit() — host framebuffer has no blitter to wait for */
}

static void graphics_WaitBOVP(void)
{
    /* WaitBOVP(vp) — A0 = vp; wait until the beam passes the bottom of the
     * ViewPort, approximated by the next VBlank tick. */
    (void)m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t start = chip_emu_vblank_count();
    while (chip_emu_vblank_count() == start) {
        Task_Yield();
    }
}

static void graphics_VBeamPos(void)
{
    /* VBeamPos() — returns 0; host display has no beam position */
    m68k_set_reg(M68K_REG_D0, 0);
}

/* =========================================================================
 * Advanced graphics support (tag-list helpers, scaling, internals)
 * ========================================================================= */

#define TAG_DONE 0
#define TAG_END  0

/* RastPort attribute tags (AmigaOS RPTAG_* values) */
#define RPTAG_FGPEN      0x80000000
#define RPTAG_BGPEN      0x80000001
#define RPTAG_DRAWMODE   0x80000002
#define RPTAG_BITMAP     0x80000003
#define RPTAG_WIDTH      0x80000004
#define RPTAG_HEIGHT     0x80000005

static uint32_t taglist_get(uint32_t tags, uint32_t tag)
{
    if (!tags) return 0;
    for (uint32_t p = tags; ; p += 8) {
        uint32_t t = m68k_read_memory_32(p);
        uint32_t v = m68k_read_memory_32(p + 4);
        if (t == TAG_DONE || t == TAG_END) return 0;
        if (t == tag) return v;
    }
}

static void taglist_set(uint32_t tags, uint32_t tag, uint32_t value)
{
    if (!tags) return;
    for (uint32_t p = tags; ; p += 8) {
        uint32_t t = m68k_read_memory_32(p);
        if (t == TAG_DONE || t == TAG_END) return;
        if (t == tag) {
            m68k_write_memory_32(p + 4, value);
            return;
        }
    }
}

/* Gfx internal object store (simple linked list of associations) */
#define GFXOBJ_SIZE 16

static uint32_t gfxobj_create(void)
{
    uint32_t obj = 0;
    dos_AllocMem_glue(GFXOBJ_SIZE, 0, &obj);
    if (obj) {
        for (int i = 0; i < GFXOBJ_SIZE; i++) m68k_write_memory_8(obj + i, 0);
    }
    return obj;
}

static void gfxobj_destroy(uint32_t obj)
{
    if (obj) dos_FreeMem_glue(obj, GFXOBJ_SIZE);
}

static void graphics_GfxNew(void)
{
    /* GfxNew(type) — D0 = node type
     * Returns a new graphics node in D0.
     */
    (void)m68k_get_reg(NULL, M68K_REG_D0);
    m68k_set_reg(M68K_REG_D0, gfxobj_create());
}

static void graphics_GfxFree(void)
{
    /* GfxFree(node) — A0 = node */
    uint32_t node = m68k_get_reg(NULL, M68K_REG_A0);
    gfxobj_destroy(node);
}

static void graphics_GfxAssociate(void)
{
    /* GfxAssociate(source, dest) — A0 = source, A1 = dest
     * Stores a single association pointer in the source node.
     */
    uint32_t src = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t dst = m68k_get_reg(NULL, M68K_REG_A1);
    if (src) m68k_write_memory_32(src + 8, dst);
}

static void graphics_GfxLookUp(void)
{
    /* GfxLookUp(node) — A0 = node
     * Returns the associated pointer in D0.
     */
    uint32_t node = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t val = node ? m68k_read_memory_32(node + 8) : 0;
    m68k_set_reg(M68K_REG_D0, val);
}

static void graphics_SetRPAttrsA(void)
{
    /* SetRPAttrsA(rp, tags) — A1 = rp, A0 = tags */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t tags = m68k_get_reg(NULL, M68K_REG_A0);
    if (!rp || !tags) return;

    for (uint32_t p = tags; ; p += 8) {
        uint32_t tag = m68k_read_memory_32(p);
        uint32_t val = m68k_read_memory_32(p + 4);
        if (tag == TAG_DONE || tag == TAG_END) break;
        switch (tag) {
            case RPTAG_FGPEN:     rp_w_u8(rp, RP_OFF_FGPEN, (uint8_t)val); break;
            case RPTAG_BGPEN:     rp_w_u8(rp, RP_OFF_BGPEN, (uint8_t)val); break;
            case RPTAG_DRAWMODE:  rp_w_u8(rp, RP_OFF_DRAWMODE, (uint8_t)val); break;
            case RPTAG_BITMAP:    m68k_write_memory_32(rp + RP_OFF_BITMAP, val); break;
            default: break;
        }
    }
}

static void graphics_GetRPAttrsA(void)
{
    /* GetRPAttrsA(rp, tags) — A1 = rp, A0 = tags */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t tags = m68k_get_reg(NULL, M68K_REG_A0);
    if (!rp || !tags) return;

    for (uint32_t p = tags; ; p += 8) {
        uint32_t tag = m68k_read_memory_32(p);
        if (tag == TAG_DONE || tag == TAG_END) break;
        uint32_t val = 0;
        switch (tag) {
            case RPTAG_FGPEN:     val = rp_u8(rp, RP_OFF_FGPEN); break;
            case RPTAG_BGPEN:     val = rp_u8(rp, RP_OFF_BGPEN); break;
            case RPTAG_DRAWMODE:  val = rp_u8(rp, RP_OFF_DRAWMODE); break;
            case RPTAG_BITMAP:    val = m68k_read_memory_32(rp + RP_OFF_BITMAP); break;
            default: break;
        }
        m68k_write_memory_32(p + 4, val);
    }
}

static void graphics_CalcIVG(void)
{
    /* CalcIVG(vp, width, height) — A0 = vp, D0 = width, D1 = height
     * Returns an interleaved value in D0.  UAOS uses chunky BitMaps so
     * this is not meaningful; return 0.
     */
    (void)m68k_get_reg(NULL, M68K_REG_A0);
    (void)m68k_get_reg(NULL, M68K_REG_D0);
    (void)m68k_get_reg(NULL, M68K_REG_D1);
    m68k_set_reg(M68K_REG_D0, 0);
}

static void graphics_AllocDBufInfo(void)
{
    /* AllocDBufInfo(vp) — A0 = vp
     * Returns a DBufInfo pointer in D0.
     */
    uint32_t vp = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t db = 0;
    dos_AllocMem_glue(32, 0, &db);
    if (db) {
        for (int i = 0; i < 32; i++) m68k_write_memory_8(db + i, 0);
    }
    m68k_set_reg(M68K_REG_D0, db);
}

static void graphics_FreeDBufInfo(void)
{
    /* FreeDBufInfo(db) — A0 = db */
    uint32_t db = m68k_get_reg(NULL, M68K_REG_A0);
    if (db) dos_FreeMem_glue(db, 32);
}

static void graphics_WeightAMatch(void)
{
    /* WeightAMatch(textAttr, textAttr, oldTextAttr, newTextAttr, flags)
     * A0, A1, A2, A3 = textAttr, D0 = flags
     * Returns a match weight in D0.  UAOS has only one font, so return 0.
     */
    (void)m68k_get_reg(NULL, M68K_REG_A0);
    (void)m68k_get_reg(NULL, M68K_REG_A1);
    (void)m68k_get_reg(NULL, M68K_REG_A2);
    (void)m68k_get_reg(NULL, M68K_REG_A3);
    (void)m68k_get_reg(NULL, M68K_REG_D0);
    m68k_set_reg(M68K_REG_D0, 0);
}

/* BitMapScaleArgs offsets (simplified) */
#define BSA_SRCBITMAP   0
#define BSA_DESTBITMAP  4
#define BSA_SRCX        8
#define BSA_SRCY        10
#define BSA_SRCWIDTH    12
#define BSA_SRCHEIGHT   14
#define BSA_DESTX       16
#define BSA_DESTY       18
#define BSA_DESTWIDTH   20
#define BSA_DESTHEIGHT  22
#define BSA_XSRCFACTOR  24
#define BSA_XDESTFACTOR 25
#define BSA_YSRCFACTOR  26
#define BSA_YDESTFACTOR 27

static void graphics_BitMapScale(void)
{
    /* BitMapScale(scaleArgs) — A0 = BitMapScaleArgs
     * Simple nearest-neighbour scale from source to destination BitMap.
     */
    uint32_t args = m68k_get_reg(NULL, M68K_REG_A0);
    if (!args) return;

    uint32_t src_bm = m68k_read_memory_32(args + BSA_SRCBITMAP);
    uint32_t dst_bm = m68k_read_memory_32(args + BSA_DESTBITMAP);
    if (!src_bm || !dst_bm) return;

    int sx = (int)m68k_read_memory_16(args + BSA_SRCX);
    int sy = (int)m68k_read_memory_16(args + BSA_SRCY);
    int sw = (int)m68k_read_memory_16(args + BSA_SRCWIDTH);
    int sh = (int)m68k_read_memory_16(args + BSA_SRCHEIGHT);
    int dx = (int)m68k_read_memory_16(args + BSA_DESTX);
    int dy = (int)m68k_read_memory_16(args + BSA_DESTY);
    int dw = (int)m68k_read_memory_16(args + BSA_DESTWIDTH);
    int dh = (int)m68k_read_memory_16(args + BSA_DESTHEIGHT);

    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    BlitSurface src, dst;
    blit_surface_from_bitmap(&src, src_bm);
    blit_surface_from_bitmap(&dst, dst_bm);

    for (int y = 0; y < dh; y++) {
        int src_y = sy + (y * sh) / dh;
        for (int x = 0; x < dw; x++) {
            int src_x = sx + (x * sw) / dw;
            uint32_t p = 0;
            blit_surface_get(&src, src_x, src_y, &p);
            blit_surface_put(&dst, dx + x, dy + y, p);
        }
    }
}

static void graphics_ScalerDiv(void)
{
    /* ScalerDiv(factor, numerator, denominator) — D0, D1, D2
     * Returns (factor * numerator) / denominator.
     */
    uint32_t factor = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t numer  = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t denom  = m68k_get_reg(NULL, M68K_REG_D2);
    if (denom == 0) { m68k_set_reg(M68K_REG_D0, 0); return; }
    m68k_set_reg(M68K_REG_D0, (factor * numer) / denom);
}

static void graphics_SetChipRev(void)
{
    /* SetChipRev(rev) — D0 = requested revision
     * Returns the actual chip revision in D0.
     * UAOS reports ECS (2) with no special flags.
     */
    (void)m68k_get_reg(NULL, M68K_REG_D0);
    m68k_set_reg(M68K_REG_D0, 2);
}

static void graphics_GetExtSpriteA(void)
{
    /* GetExtSpriteA(sprite, tags) — A0 = sprite, A1 = tags
     * Returns 0 in D0 (no extended sprite data).
     */
    (void)m68k_get_reg(NULL, M68K_REG_A0);
    (void)m68k_get_reg(NULL, M68K_REG_A1);
    m68k_set_reg(M68K_REG_D0, 0);
}

/* =========================================================================
 * Function table indexed by LVO slot (|LVO| / 6).
 *
 * The AmigaOS graphics.library jump table runs from LVO -30 (slot 5) to
 * LVO -1056 (slot 176).  Unimplemented slots share graphics_Unimplemented.
 * ========================================================================= */

static void *graphics_funcs[GFX_SLOT_MAX + 1] = {
    [GFX_SLOT_BLITBITMAP]              = graphics_BltBitMap,
    [GFX_SLOT_BLTTEMPLATE]             = graphics_BltTemplate,
    [GFX_SLOT_CLEAREOL]                = graphics_ClearEOL,
    [GFX_SLOT_CLEARSCREEN]             = graphics_ClearScreen,
    [GFX_SLOT_TEXTLENGTH]              = graphics_TextLength,
    [GFX_SLOT_TEXT]                    = graphics_Text,
    [GFX_SLOT_SETFONT]                 = graphics_SetFont,
    [GFX_SLOT_OPENFONT]                = graphics_OpenFont,
    [GFX_SLOT_CLOSEFONT]               = graphics_CloseFont,
    [GFX_SLOT_ASKSOFTSTYLE]            = graphics_AskSoftStyle,
    [GFX_SLOT_SETSOFTSTYLE]            = graphics_SetSoftStyle,
    [GFX_SLOT_LOADRGB4]                = graphics_LoadRGB4,
    [GFX_SLOT_INITRASTPORT]            = graphics_InitRastPort,
    [GFX_SLOT_INITVPORT]               = graphics_InitVPort,
    [GFX_SLOT_MRGCOP]                  = graphics_MrgCop,
    [GFX_SLOT_MAKEVPORT]               = graphics_MakeVPort,
    [GFX_SLOT_LOADVIEW]                = graphics_LoadView,
    [GFX_SLOT_WAITBLIT]                = graphics_WaitBlit,
    [GFX_SLOT_SETRAST]                 = graphics_SetRast,
    [GFX_SLOT_MOVE]                    = graphics_Move,
    [GFX_SLOT_DRAW]                    = graphics_Draw,
    [GFX_SLOT_AREAMOVE]                = graphics_AreaMove,
    [GFX_SLOT_AREADRAW]                = graphics_AreaDraw,
    [GFX_SLOT_AREAEND]                 = graphics_AreaEnd,
    [GFX_SLOT_WAITTOF]                 = graphics_WaitTOF,
    [GFX_SLOT_INITAREA]                = graphics_InitArea,
    [GFX_SLOT_SETRGB4]                 = graphics_SetRGB4,
    [GFX_SLOT_BLTPATTERN]              = graphics_BltPattern,
    [GFX_SLOT_BLTCLEAR]                = graphics_BltClear,
    [GFX_SLOT_GFXNEW]                  = graphics_GfxNew,
    [GFX_SLOT_GFXFREE]                 = graphics_GfxFree,
    [GFX_SLOT_GFXASSOCIATE]            = graphics_GfxAssociate,
    [GFX_SLOT_BITMAPSCALE]             = graphics_BitMapScale,
    [GFX_SLOT_SCALERDIV]               = graphics_ScalerDiv,
    [GFX_SLOT_LOCKLAYERROM]            = graphics_LockLayerRom,
    [GFX_SLOT_UNLOCKLAYERROM]          = graphics_UnlockLayerRom,
    [GFX_SLOT_SYNCSBITMAP]             = graphics_SyncSBitMap,
    [GFX_SLOT_COPYSBITMAP]             = graphics_CopySBitMap,
    [GFX_SLOT_ANDRECTREGION]           = graphics_AndRectRegion,
    [GFX_SLOT_ORRECTREGION]            = graphics_OrRectRegion,
    [GFX_SLOT_NEWREGION]               = graphics_NewRegion,
    [GFX_SLOT_CLEARRECTREGION]         = graphics_ClearRectRegion,
    [GFX_SLOT_CLEARREGION]             = graphics_ClearRegion,
    [GFX_SLOT_DISPOSEREGION]           = graphics_DisposeRegion,
    [GFX_SLOT_RECTFILL]                = graphics_RectFill,
    [GFX_SLOT_FLOOD]                   = graphics_Flood,
    [GFX_SLOT_POLYDRAW]                = graphics_PolyDraw,
    [GFX_SLOT_READPIXEL]               = graphics_ReadPixel,
    [GFX_SLOT_WRITEPIXEL]              = graphics_WritePixel,
    [GFX_SLOT_XORRECTREGION]           = graphics_XorRectRegion,
    [GFX_SLOT_SETAPEN]                 = graphics_SetAPen,
    [GFX_SLOT_SETBPEN]                 = graphics_SetBPen,
    [GFX_SLOT_SETDRMD]                 = graphics_SetDrMd,
    [GFX_SLOT_INITVIEW]                = graphics_InitView,
    [GFX_SLOT_DRAWELLIPSE]             = graphics_DrawEllipse,
    [GFX_SLOT_AREAELLIPSE]             = graphics_AreaEllipse,
    [GFX_SLOT_VBEAMPOS]                = graphics_VBeamPos,
    [GFX_SLOT_INITBITMAP]              = graphics_InitBitMap,
    [GFX_SLOT_SCROLLRASTER]            = graphics_ScrollRaster,
    [GFX_SLOT_WAITBOVP]                = graphics_WaitBOVP,
    [GFX_SLOT_GETCOLORMAP]             = graphics_GetColorMap,
    [GFX_SLOT_FREECOLORMAP]            = graphics_FreeColorMap,
    [GFX_SLOT_GETRGB4]                 = graphics_GetRGB4,
    [GFX_SLOT_ASKFONT]                 = graphics_AskFont,
    [GFX_SLOT_ADDFONT]                 = graphics_AddFont,
    [GFX_SLOT_REMFONT]                 = graphics_RemFont,
    [GFX_SLOT_ALLOCRASTER]             = graphics_AllocRaster,
    [GFX_SLOT_FREERASTER]              = graphics_FreeRaster,
    [GFX_SLOT_INITTMPRAS]              = graphics_InitTmpRas,
    [GFX_SLOT_TEXTFIT]                 = graphics_TextFit,
    [GFX_SLOT_GFXLOOKUP]               = graphics_GfxLookUp,
    [GFX_SLOT_SETRGB4CM]               = graphics_SetRGB4CM,
    [GFX_SLOT_CLIPBLIT]                = graphics_ClipBlt,
    [GFX_SLOT_ORREGIONREGION]          = graphics_OrRegionRegion,
    [GFX_SLOT_XORREGIONREGION]         = graphics_XorRegionRegion,
    [GFX_SLOT_ANDREGIONREGION]         = graphics_AndRegionRegion,
    [GFX_SLOT_BLTBITMAPRASTPORT]       = graphics_BltBitMapRastPort,
    [GFX_SLOT_GETVPMODEID]             = graphics_GetVPModeID,
    [GFX_SLOT_MODENOTAVAILABLE]        = graphics_ModeNotAvailable,
    [GFX_SLOT_WEIGHTAMATCH]            = graphics_WeightAMatch,
    [GFX_SLOT_CALCIVG]                 = graphics_CalcIVG,
    [GFX_SLOT_ATTACHPALEXTRA]          = graphics_AttachPalExtra,
    [GFX_SLOT_OBTAINBESTPENA]          = graphics_ObtainBestPenA,
    [GFX_SLOT_VIDEOCONTROL]            = graphics_VideoControl,
    [GFX_SLOT_ATTEMPTLOCKLAYERROM]     = graphics_AttemptLockLayerRom,
    [GFX_SLOT_OPENMONITOR]             = graphics_OpenMonitor,
    [GFX_SLOT_CLOSEMONITOR]            = graphics_CloseMonitor,
    [GFX_SLOT_FINDDISPLAYINFO]         = graphics_FindDisplayInfo,
    [GFX_SLOT_NEXTDISPLAYINFO]         = graphics_NextDisplayInfo,
    [GFX_SLOT_GETDISPLAYINFODATA]      = graphics_GetDisplayInfoData,
    [GFX_SLOT_SETRGB32]                = graphics_SetRGB32,
    [GFX_SLOT_GETAPEN]                 = graphics_GetAPen,
    [GFX_SLOT_GETBPEN]                 = graphics_GetBPen,
    [GFX_SLOT_GETDRMD]                 = graphics_GetDrMd,
    [GFX_SLOT_GETOUTLINEPEN]           = graphics_GetOutlinePen,
    [GFX_SLOT_LOADRGB32]               = graphics_LoadRGB32,
    [GFX_SLOT_GETRGB32]                = graphics_GetRGB32,
    [GFX_SLOT_SETABPENDRMD]            = graphics_SetABPenDrMd,
    [GFX_SLOT_BLTMASKBITMAPRASTPORT]   = graphics_BltMaskBitMapRastPort,
    [GFX_SLOT_SETCHIPREV]              = graphics_SetChipRev,
    [GFX_SLOT_GETEXTSPRITEA]           = graphics_GetExtSpriteA,
    [GFX_SLOT_COERCEMODE]              = graphics_CoerceMode,
    [GFX_SLOT_ALLOCBITMAP]             = graphics_AllocBitMap,
    [GFX_SLOT_FREEBITMAP]              = graphics_FreeBitMap,
    [GFX_SLOT_CHANGEVPBITMAP]          = graphics_ChangeVPBitMap,
    [GFX_SLOT_GETBITMAPATTR]           = graphics_GetBitMapAttr,
    [GFX_SLOT_ALLOCDBUFINFO]           = graphics_AllocDBufInfo,
    [GFX_SLOT_FREEDBUFINFO]            = graphics_FreeDBufInfo,
    [GFX_SLOT_RELEASEPEN]              = graphics_ReleasePen,
    [GFX_SLOT_OBTAINPEN]               = graphics_ObtainPen,
    [GFX_SLOT_READPIXELLINE8]          = graphics_ReadPixelLine8,
    [GFX_SLOT_WRITEPIXELLINE8]         = graphics_WritePixelLine8,
    [GFX_SLOT_READPIXELARRAY8]         = graphics_ReadPixelArray8,
    [GFX_SLOT_WRITEPIXELARRAY8]        = graphics_WritePixelArray8,
    [GFX_SLOT_TEXTEXTENT]              = graphics_TextExtent,
    [GFX_SLOT_FONTEXTENT]              = graphics_FontExtent,
    [GFX_SLOT_ERASERECT]               = graphics_EraseRect,
    [GFX_SLOT_EXTENDFONT]              = graphics_ExtendFont,
    [GFX_SLOT_STRIPFONT]               = graphics_StripFont,
    [GFX_SLOT_SETOUTLINEPEN]           = graphics_SetOutlinePen,
    [GFX_SLOT_SETWRITEMASK]            = graphics_SetWriteMask,
    [GFX_SLOT_SETMAXPEN]               = graphics_SetMaxPen,
    [GFX_SLOT_SETRGB32CM]              = graphics_SetRGB32CM,
    [GFX_SLOT_BESTMODEIDA]             = graphics_BestModeIDA,
    [GFX_SLOT_SCROLLRASTERBF]          = graphics_ScrollRasterBF,
    [GFX_SLOT_FINDCOLOR]               = graphics_FindColor,
    [GFX_SLOT_SETRPATTRSA]             = graphics_SetRPAttrsA,
    [GFX_SLOT_GETRPATTRSA]             = graphics_GetRPAttrsA,
    [GFX_SLOT_WRITECHUNKYPIXELS]         = graphics_WriteChunkyPixels,
};

/* =========================================================================
 * Musashi dispatch entry point (called from uaos_m68k_glue.c)
 *
 * fn is the LVO slot number (|LVO| / 6) as encoded by install_lvo().
 * ========================================================================= */

void UAOS_Graphics_Dispatch(uint32_t fn)
{
    if (fn < GFX_SLOT_MIN || fn > GFX_SLOT_MAX) {
        fprintf(stderr, "[GRAPHICS] unknown LVO slot %u\n", fn);
        return;
    }
    void (*fp)(void) = graphics_funcs[fn];
    if (fp) fp();
    else graphics_Unimplemented();
}

/* =========================================================================
 * Registration function
 * ========================================================================= */

void UAOS_GRAPHICS_Register(void)
{
    UAOS_ROM_Register("graphics.library", 40, 0x000000C0,
                      (uint16_t)(sizeof(graphics_funcs) / sizeof(graphics_funcs[0])),
                      graphics_funcs);
}
