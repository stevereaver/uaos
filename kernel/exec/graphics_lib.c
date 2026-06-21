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
#include "../display/framebuffer.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

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
#define M68K_REG_A0  8
#define M68K_REG_A1  9

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

static uint32_t current_fg(uint32_t rp)
{
    if (!rp) return amiga_pen_to_rgb(1); /* default white */
    return amiga_pen_to_rgb(rp_u8(rp, RP_OFF_FGPEN));
}

static uint32_t current_bg(uint32_t rp)
{
    if (!rp) return amiga_pen_to_rgb(0); /* default black */
    return amiga_pen_to_rgb(rp_u8(rp, RP_OFF_BGPEN));
}

static int current_mode(uint32_t rp)
{
    if (!rp) return JAM1;
    return (int)rp_u8(rp, RP_OFF_DRAWMODE);
}

/* Bresenham line — clipped per-pixel by FB_PutPixel */
static void draw_line(int x0, int y0, int x1, int y1, uint32_t colour)
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
            FB_PutPixel(x, y, colour);
            err -= dy;
            if (err < 0) { y += sy; err += dx; }
        }
    } else {
        int err = dy / 2;
        for (int x = x0, y = y0, i = 0; i <= dy; i++, y += sy) {
            FB_PutPixel(x, y, colour);
            err -= dx;
            if (err < 0) { x += sx; err += dy; }
        }
    }
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
    /* InitView — stub, we have no copper/display hardware */
}

static void graphics_LoadView(void)
{
    /* LoadView — stub */
}

static void graphics_WaitTOF(void)
{
    /* WaitTOF — stub, host display has no hardware VBlank to wait for */
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
    int mode    = current_mode(rp);

    /* Render each character using the fixed 8×16 font.
     * For JAM1 we draw only fg pixels; for JAM2 we draw bg too.  */
    for (int i = 0; i < len; i++) {
        char ch = (char)m68k_read_memory_8(str + i);
        if (mode == JAM1) {
            /* Draw character with transparent background:
             * FB_PutChar always draws a full char cell with bg colour,
             * so we need a different approach.  For now, use the bg
             * pen because the framebuffer font renderer doesn't support
             * transparent bg directly.  This is a known limitation. */
            FB_PutChar(x, y, ch, fg, bg);
        } else {
            FB_PutChar(x, y, ch, fg, bg);
        }
        x += FB_CharWidth();
    }

    rp_w_s16(rp, RP_OFF_CP_X, (int16_t)x);
}

static void graphics_TextFit(void)
{
    /* TextFit — stub, return 0 */
    m68k_set_reg(M68K_REG_D0, 0);
}

static void graphics_TextLength(void)
{
    /* TextLength(rp, string, length)
     * A1 = rp, A0 = string, D0 = length
     * Returns pixel width in D0.
     */
    int len = (int)m68k_get_reg(NULL, M68K_REG_D0);
    if (len < 0) len = 0;
    m68k_set_reg(M68K_REG_D0, (unsigned int)(len * FB_CharWidth()));
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
    draw_line(x0, y0, x1, y1, col);

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
    /* FB_FillRect takes x, y, w, h */
    FB_FillRect(x1, y1, x2 - x1 + 1, y2 - y1 + 1, col);
}

static void graphics_SetRast(void)
{
    /* SetRast — fill the entire rasterport bitmap with BgPen (stub: fill screen) */
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t col = current_bg(rp);
    if (g_fb.valid) {
        FB_FillRect(0, 0, (int)g_fb.width, (int)g_fb.height, col);
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
    /* BltBitMap — stub */
}

static void graphics_BltTemplate(void)
{
    /* BltTemplate — stub */
}

static void graphics_ReadPixel(void)
{
    /* ReadPixel(rp, x, y)
     * A1 = rp, D0 = x, D1 = y
     * Returns colour in D0.
     */
    int x = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int y = (int)m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t col = FB_GetPixel(x, y);
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
    FB_PutPixel(x, y, col);
}

static void graphics_GetBitMapAttr(void)
{
    /* GetBitMapAttr — stub */
    m68k_set_reg(M68K_REG_D0, 0);
}

static void graphics_AllocBitMap(void)
{
    /* AllocBitMap — stub */
    m68k_set_reg(M68K_REG_D0, 0);
}

static void graphics_FreeBitMap(void)
{
    /* FreeBitMap — stub */
}

static void graphics_LoadRGB4(void)
{
    /* LoadRGB4 — stub */
}

static void graphics_LoadRGB32(void)
{
    /* LoadRGB32 — stub */
}

static void graphics_GetColorMap(void)
{
    /* GetColorMap — stub */
    m68k_set_reg(M68K_REG_D0, 0);
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
     * No-op on our truecolour framebuffer; kept as a safe stub.
     */
    (void)m68k_get_reg(NULL, M68K_REG_A1);
    (void)m68k_get_reg(NULL, M68K_REG_D0);
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
     * No-op: host framebuffer has a fixed 32-bit palette.
     */
    (void)m68k_get_reg(NULL, M68K_REG_A0);
    (void)m68k_get_reg(NULL, M68K_REG_D0);
    (void)m68k_get_reg(NULL, M68K_REG_D1);
    (void)m68k_get_reg(NULL, M68K_REG_D2);
    (void)m68k_get_reg(NULL, M68K_REG_D3);
}

static void graphics_WaitBlit(void)
{
    /* WaitBlit() — host framebuffer has no blitter to wait for */
}

static void graphics_WaitBOVP(void)
{
    /* WaitBOVP(vp) — A0 = vp; no hardware beam sync */
    (void)m68k_get_reg(NULL, M68K_REG_A0);
}

static void graphics_VBeamPos(void)
{
    /* VBeamPos() — returns 0; host display has no beam position */
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
    [GFX_SLOT_TEXTLENGTH]              = graphics_TextLength,
    [GFX_SLOT_TEXT]                    = graphics_Text,
    [GFX_SLOT_LOADRGB4]                = graphics_LoadRGB4,
    [GFX_SLOT_INITRASTPORT]            = graphics_InitRastPort,
    [GFX_SLOT_LOADVIEW]                = graphics_LoadView,
    [GFX_SLOT_WAITBLIT]                = graphics_WaitBlit,
    [GFX_SLOT_SETRAST]                 = graphics_SetRast,
    [GFX_SLOT_MOVE]                    = graphics_Move,
    [GFX_SLOT_DRAW]                    = graphics_Draw,
    [GFX_SLOT_WAITTOF]                 = graphics_WaitTOF,
    [GFX_SLOT_SETRGB4]                 = graphics_SetRGB4,
    [GFX_SLOT_RECTFILL]                = graphics_RectFill,
    [GFX_SLOT_READPIXEL]               = graphics_ReadPixel,
    [GFX_SLOT_WRITEPIXEL]              = graphics_WritePixel,
    [GFX_SLOT_SETAPEN]                 = graphics_SetAPen,
    [GFX_SLOT_SETBPEN]                 = graphics_SetBPen,
    [GFX_SLOT_SETDRMD]                 = graphics_SetDrMd,
    [GFX_SLOT_INITVIEW]                = graphics_InitView,
    [GFX_SLOT_VBEAMPOS]                = graphics_VBeamPos,
    [GFX_SLOT_WAITBOVP]                = graphics_WaitBOVP,
    [GFX_SLOT_GETCOLORMAP]             = graphics_GetColorMap,
    [GFX_SLOT_TEXTFIT]                 = graphics_TextFit,
    [GFX_SLOT_SETRGB32]                = graphics_LoadRGB32,
    [GFX_SLOT_GETAPEN]                 = graphics_GetAPen,
    [GFX_SLOT_GETBPEN]                 = graphics_GetBPen,
    [GFX_SLOT_GETDRMD]                 = graphics_GetDrMd,
    [GFX_SLOT_GETOUTLINEPEN]           = graphics_GetOutlinePen,
    [GFX_SLOT_LOADRGB32]               = graphics_LoadRGB32,
    [GFX_SLOT_SETABPENDRMD]            = graphics_SetABPenDrMd,
    [GFX_SLOT_ALLOCBITMAP]             = graphics_AllocBitMap,
    [GFX_SLOT_FREEBITMAP]              = graphics_FreeBitMap,
    [GFX_SLOT_GETBITMAPATTR]           = graphics_GetBitMapAttr,
    [GFX_SLOT_SETOUTLINEPEN]           = graphics_SetOutlinePen,
    [GFX_SLOT_SETWRITEMASK]            = graphics_SetWriteMask,
    [GFX_SLOT_SETMAXPEN]               = graphics_SetMaxPen,
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
