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
 * Helpers: pen → RGB, line drawing
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
 * graphics.library function indices (must match AmigaOS LVO offsets)
 * ========================================================================= */

#define GRAPHICS_OPEN_LIBRARY   1
#define GRAPHICS_CLOSE_LIBRARY  2
#define GRAPHICS_INIT_RASTPORT  3
#define GRAPHICS_INIT_VIEW     4
#define GRAPHICS_LOAD_VIEW     5
#define GRAPHICS_WAITTOF       6
#define GRAPHICS_RASTPORT      7
#define GRAPHICS_TEXT          8
#define GRAPHICS_TEXTFIT       9
#define GRAPHICS_TEXT_LENGTH   10
#define GRAPHICS_MOVE          11
#define GRAPHICS_DRAW          12
#define GRAPHICS_RECTFILL      13
#define GRAPHICS_POLYGON       14
#define GRAPHICS_ELLIPSE       15
#define GRAPHICS_SET_RAST      16
#define GRAPHICS_SET_APEN      17
#define GRAPHICS_SET_BPEN      18
#define GRAPHICS_SET_DRMD      19
#define GRAPHICS_SET_OPEN      20
#define GRAPHICS_SET_WRITE_MASK 21
#define GRAPHICS_BLIT          22
#define GRAPHICS_READ_PIXEL    23
#define GRAPHICS_WRITE_PIXEL   24
#define GRAPHICS_GET_BITMAP    25
#define GRAPHICS_ALLOC_BITMAP  26
#define GRAPHICS_FREE_BITMAP   27
#define GRAPHICS_LOAD_RGB4    28
#define GRAPHICS_LOAD_RGB32   29
#define GRAPHICS_GET_COLOR_MAP 30

/* =========================================================================
 * Implementation
 * ========================================================================= */

static void graphics_OpenLibrary(void)
{
    /* OpenLibrary — return library base (set by exec_OpenLibrary in glue) */
}

static void graphics_CloseLibrary(void)
{
    /* CloseLibrary — no-op for ROM library */
}

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

static void graphics_RastPort(void)
{
    /* RastPort — placeholder, not a standard graphics function */
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

static void graphics_Polygon(void)
{
    /* Polygon — stub */
}

static void graphics_Ellipse(void)
{
    /* Ellipse — stub */
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

static void graphics_SetOPen(void)
{
    /* SetOPen — stub */
}

static void graphics_SetWriteMask(void)
{
    /* SetWriteMask — stub */
}

static void graphics_Blit(void)
{
    /* Blit — stub */
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

static void graphics_GetBitMap(void)
{
    /* GetBitMap — stub */
}

static void graphics_AllocBitMap(void)
{
    /* AllocBitMap — stub */
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
}

/* =========================================================================
 * Function table (ROM module registry, consumed by thunk_handler.c)
 * ========================================================================= */

static void *graphics_funcs[] = {
    graphics_OpenLibrary,   /* index 1  */
    graphics_CloseLibrary,  /* index 2  */
    graphics_InitRastPort,  /* index 3  */
    graphics_InitView,     /* index 4  */
    graphics_LoadView,     /* index 5  */
    graphics_WaitTOF,      /* index 6  */
    graphics_RastPort,     /* index 7  */
    graphics_Text,         /* index 8  */
    graphics_TextFit,      /* index 9  */
    graphics_TextLength,   /* index 10 */
    graphics_Move,         /* index 11 */
    graphics_Draw,         /* index 12 */
    graphics_RectFill,     /* index 13 */
    graphics_Polygon,      /* index 14 */
    graphics_Ellipse,      /* index 15 */
    graphics_SetRast,      /* index 16 */
    graphics_SetAPen,     /* index 17 */
    graphics_SetBPen,     /* index 18 */
    graphics_SetDrMd,      /* index 19 */
    graphics_SetOPen,      /* index 20 */
    graphics_SetWriteMask, /* index 21 */
    graphics_Blit,         /* index 22 */
    graphics_ReadPixel,    /* index 23 */
    graphics_WritePixel,   /* index 24 */
    graphics_GetBitMap,    /* index 25 */
    graphics_AllocBitMap,  /* index 26 */
    graphics_FreeBitMap,   /* index 27 */
    graphics_LoadRGB4,     /* index 28 */
    graphics_LoadRGB32,    /* index 29 */
    graphics_GetColorMap,  /* index 30 */
};

/* =========================================================================
 * Musashi dispatch entry point (called from uaos_m68k_glue.c)
 * ========================================================================= */

void UAOS_Graphics_Dispatch(uint32_t fn)
{
    if (fn == 0 || fn > 30) {
        fprintf(stderr, "[GRAPHICS] unknown fn=%u\n", fn);
        return;
    }
    void (*fp)(void) = graphics_funcs[fn - 1];
    if (fp) fp();
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
