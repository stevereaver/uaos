/* cursor.c — UAOS Framebuffer Software Cursor
 *
 * Implements a configurable Amiga-style cursor with multiple sizes,
 * customizable colors, and visibility options.
 *
 * Cursor rendering uses a two-pass technique:
 *   1. Save the pixels currently under the cursor into a background buffer
 *   2. Draw the cursor sprite (body color + shadow color)
 *
 * On move: restore saved background, save new background, draw at new pos.
 *
 * Supported cursor sizes: 16x16, 32x32, 48x48
 * Customizable colors: body, shadow, background
 * Visibility options: double pixel mode, mouse acceleration
 */

#include "cursor.h"
#include "framebuffer.h"
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Cursor sprite definitions
 * ========================================================================= */

#define CUR_MAX_W  48
#define CUR_MAX_H  48

/* Pixel values */
#define _ 0  /* transparent */
#define B 1  /* shadow/outline */
#define W 2  /* body */

/* 16x16 Amiga-style arrow pointer */
static const uint8_t cur_16x16[16][16] = {
/*       0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 */
/* r0 */ W, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r1 */ W, W, B, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r2 */ W, W, W, B, _, _, _, _, _, _, _, _, _, _, _, _,
/* r3 */ W, W, W, W, B, _, _, _, _, _, _, _, _, _, _, _,
/* r4 */ W, W, W, W, W, B, _, _, _, _, _, _, _, _, _, _,
/* r5 */ W, W, W, W, W, W, B, _, _, _, _, _, _, _, _, _,
/* r6 */ W, W, W, W, W, W, W, B, _, _, _, _, _, _, _, _,
/* r7 */ W, W, W, W, W, W, W, W, B, _, _, _, _, _, _, _,
/* r8 */ W, W, W, W, W, W, W, W, W, B, _, _, _, _, _, _,
/* r9 */ W, W, W, W, W, W, B, B, B, B, _, _, _, _, _, _,
/*r10 */ W, W, W, B, W, W, B, _, _, _, _, _, _, _, _, _,
/*r11 */ W, W, B, _, B, W, W, B, _, _, _, _, _, _, _, _,
/*r12 */ W, B, _, _, _, B, W, W, B, _, _, _, _, _, _, _,
/*r13 */ B, _, _, _, _, _, B, W, W, B, _, _, _, _, _, _,
/*r14 */ _, _, _, _, _, _, _, B, W, W, B, _, _, _, _, _,
/*r15 */ _, _, _, _, _, _, _, _, B, B, _, _, _, _, _, _,
};

/* 32x32 and 48x48 arrow pointers - generated at boot by integer-scaling
 * the (correct) 16x16 map above.  The previous hand-typed tables here had
 * wrong per-row element counts (hidden by -Wno-missing-braces), which
 * shifted every row left and produced skewed/garbage sprites.  Scaling the
 * verified 16x16 source guarantees a clean pixel-doubled / pixel-tripled
 * arrow.  See B1 in the framebuffer-perf plan. */
static uint8_t cur_32x32[32 * 32];
static uint8_t cur_48x48[48 * 48];
static int     cur_sprites_scaled = 0;

static void scale_sprite(const uint8_t src[16][16], uint8_t *dst, int scale)
{
    int dim = 16 * scale;
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 16; col++) {
            uint8_t p = src[row][col];
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++)
                    dst[(row * scale + dy) * dim + (col * scale + dx)] = p;
        }
    }
}

static void ensure_sprites_scaled(void)
{
    if (cur_sprites_scaled) return;
    scale_sprite(cur_16x16, cur_32x32, 2);
    scale_sprite(cur_16x16, cur_48x48, 3);
    cur_sprites_scaled = 1;
}



/* 16x16 busy pointer (hourglass-ish) */
static const uint8_t cur_busy_16x16[16][16] = {
/*       0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 */
/* r0 */ _, _, _, _, B, B, B, B, B, B, _, _, _, _, _, _,
/* r1 */ _, _, _, B, W, W, W, W, W, W, B, _, _, _, _, _,
/* r2 */ _, _, B, W, W, _, _, _, _, W, W, B, _, _, _, _,
/* r3 */ _, B, W, W, _, _, _, _, _, _, W, W, B, _, _, _,
/* r4 */ B, W, W, _, _, _, W, W, _, _, _, W, W, B, _, _,
/* r5 */ B, W, _, _, _, W, W, W, W, _, _, _, W, B, _, _,
/* r6 */ B, W, _, _, W, W, B, B, W, W, _, _, W, B, _, _,
/* r7 */ B, W, _, _, W, W, B, B, W, W, _, _, W, B, _, _,
/* r8 */ B, W, _, _, _, W, W, W, W, _, _, _, W, B, _, _,
/* r9 */ B, W, W, _, _, _, W, W, _, _, _, W, W, B, _, _,
/*r10 */ _, B, W, W, _, _, _, _, _, _, W, W, B, _, _, _,
/*r11 */ _, _, B, W, W, _, _, _, _, W, W, B, _, _, _, _,
/*r12 */ _, _, _, B, W, W, W, W, W, W, B, _, _, _, _, _,
/*r13 */ _, _, _, _, B, W, W, W, W, B, _, _, _, _, _, _,
/*r14 */ _, _, _, _, _, B, W, W, B, _, _, _, _, _, _, _,
/*r15 */ _, _, _, _, _, _, B, B, _, _, _, _, _, _, _, _,
};

#undef _
#undef B
#undef W

/* =========================================================================
 * Custom cursor state
 * ========================================================================= */

static uint8_t cur_custom[CUR_MAX_W * CUR_MAX_H];
static int     cur_custom_w = 0;
static int     cur_custom_h = 0;
static int     cur_custom_x = 0;
static int     cur_custom_y = 0;
static int     cur_custom_active = 0;
static int     cur_busy = 0;

/* =========================================================================
 * Cursor settings
 * ========================================================================= */

static CursorSettings g_cursor_settings = {
    .size = CURSOR_SIZE_16x16,
    .colors = {
        .body_color = CURSOR_DEFAULT_BODY,
        .shadow_color = CURSOR_DEFAULT_SHADOW,
        .bg_color = CURSOR_DEFAULT_BG
    },
    .acceleration = 50,
    .double_pixel = 0
};

/* =========================================================================
 * Background save buffer
 * ========================================================================= */

static uint32_t bg_save[CUR_MAX_W * CUR_MAX_H];
static int      cur_x = 0;
static int      cur_y = 0;
static int      cur_drawn = 0;   /* 1 if cursor is currently on screen */

/* =========================================================================
 * Helper functions
 * ========================================================================= */

static int get_cursor_size(void)
{
    if (cur_custom_active) return cur_custom_h;
    if (cur_busy) return 16;
    switch (g_cursor_settings.size) {
        case CURSOR_SIZE_16x16: return 16;
        case CURSOR_SIZE_32x32: return 32;
        case CURSOR_SIZE_48x48: return 48;
        default: return 16;
    }
}

static int get_cursor_width(void)
{
    if (cur_custom_active) return cur_custom_w;
    return get_cursor_size();
}

static const uint8_t* get_cursor_sprite(void)
{
    if (cur_custom_active) return cur_custom;
    if (cur_busy) return (const uint8_t*)cur_busy_16x16;
    switch (g_cursor_settings.size) {
        case CURSOR_SIZE_16x16: return (const uint8_t*)cur_16x16;
        case CURSOR_SIZE_32x32: ensure_sprites_scaled(); return (const uint8_t*)cur_32x32;
        case CURSOR_SIZE_48x48: ensure_sprites_scaled(); return (const uint8_t*)cur_48x48;
        default: return (const uint8_t*)cur_16x16;
    }
}

/* =========================================================================
 * Background save / restore
 * ========================================================================= */

static void cursor_save_bg(int x, int y)
{
    int W = (int)g_fb.width;
    int H = (int)g_fb.height;
    int cur_h = get_cursor_size();
    int cur_w = get_cursor_width();
    int off_x = cur_custom_active ? cur_custom_x : 0;
    int off_y = cur_custom_active ? cur_custom_y : 0;

    for (int row = 0; row < cur_h; row++) {
        int py = y + off_y + row;
        for (int col = 0; col < cur_w; col++) {
            int px = x + off_x + col;
            if (px < 0 || px >= W || py < 0 || py >= H)
                bg_save[row * CUR_MAX_W + col] = 0;
            else
                bg_save[row * CUR_MAX_W + col] = FB_GetPixel(px, py);
        }
    }
}

static void cursor_restore_bg(int x, int y)
{
    if (FB_IsDrawing()) return;  /* back buffer — full frame redrawn anyway */
    int W = (int)g_fb.width;
    int H = (int)g_fb.height;
    int cur_h = get_cursor_size();
    int cur_w = get_cursor_width();
    int off_x = cur_custom_active ? cur_custom_x : 0;
    int off_y = cur_custom_active ? cur_custom_y : 0;

    for (int row = 0; row < cur_h; row++) {
        int py = y + off_y + row;
        if (py < 0 || py >= H) continue;
        for (int col = 0; col < cur_w; col++) {
            int px = x + off_x + col;
            if (px < 0 || px >= W) continue;
            FB_PutPixel(px, py, bg_save[row * CUR_MAX_W + col]);
        }
    }
}

/* =========================================================================
 * Draw sprite at (x, y)
 * ========================================================================= */

static void cursor_draw(int x, int y)
{
    int W = (int)g_fb.width;
    int H = (int)g_fb.height;
    int cur_h = get_cursor_size();
    int cur_w = get_cursor_width();
    const uint8_t *sprite = get_cursor_sprite();
    uint32_t body_col = g_cursor_settings.colors.body_color;
    uint32_t shadow_col = g_cursor_settings.colors.shadow_color;
    int double_pixel = g_cursor_settings.double_pixel;
    int off_x = cur_custom_active ? cur_custom_x : 0;
    int off_y = cur_custom_active ? cur_custom_y : 0;

    for (int row = 0; row < cur_h; row++) {
        int py = y + off_y + row;
        if (py < 0 || py >= H) continue;
        for (int col = 0; col < cur_w; col++) {
            int px = x + off_x + col;
            if (px < 0 || px >= W) continue;
            uint8_t p = sprite[row * cur_w + col];

            if (p == 1) {
                FB_PutPixel(px, py, shadow_col);
                if (double_pixel && col + 1 < cur_w && px + 1 < W)
                    FB_PutPixel(px + 1, py, shadow_col);
            } else if (p == 2) {
                FB_PutPixel(px, py, body_col);
                if (double_pixel && col + 1 < cur_w && px + 1 < W)
                    FB_PutPixel(px + 1, py, body_col);
            }
            /* p == 0: transparent — leave background as-is */
        }
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void Cursor_Init(int x, int y)
{
    cur_x     = x;
    cur_y     = y;
    cur_drawn = 0;
    if (!g_fb.valid) return;
    cursor_save_bg(x, y);
    cursor_draw(x, y);
    cur_drawn = 1;
}

/* Intuition may have scheduled a delayed pointer change via WA_PointerDelay. */
extern void UAOS_Intuition_CheckPendingPointer(void);

void Cursor_Move(int x, int y)
{
    UAOS_Intuition_CheckPendingPointer();
    if (!g_fb.valid) return;
    /* B2: Cursor_Move runs at IRQ time from PS2Mouse_IRQHandler.  If a
     * back-buffered frame is in progress (FB_BeginDraw..FB_Flip), drawing
     * the cursor here would pollute the half-painted back buffer and the
     * save/restore pair would capture cursor pixels into bg_save, leaving
     * a ghost cursor on the next restore.  The frame-end Cursor_Redraw
     * paints the cursor at the new position anyway, so while drawing is
     * active we only update the stored position. */
    if (FB_IsDrawing()) {
        cur_x = x;
        cur_y = y;
        return;
    }
    if (cur_drawn)
        cursor_restore_bg(cur_x, cur_y);
    cur_x = x;
    cur_y = y;
    cursor_save_bg(x, y);
    cursor_draw(x, y);
    cur_drawn = 1;
}

void Cursor_Redraw(void)
{
    UAOS_Intuition_CheckPendingPointer();
    if (!g_fb.valid) return;
    if (cur_drawn)
        cursor_restore_bg(cur_x, cur_y);
    cursor_save_bg(cur_x, cur_y);
    cursor_draw(cur_x, cur_y);
    cur_drawn = 1;
}

void Cursor_Hide(void)
{
    if (!g_fb.valid) return;
    if (FB_IsDrawing()) return;  /* no-op during double-buffered draw */
    if (cur_drawn) {
        cursor_restore_bg(cur_x, cur_y);
        cur_drawn = 0;
    }
}

/* =========================================================================
 * Cursor settings management
 * ========================================================================= */

void Cursor_SetSize(CursorSize size)
{
    if (size >= CURSOR_SIZE_COUNT) return;
    g_cursor_settings.size = size;
}

void Cursor_SetColors(uint32_t body, uint32_t shadow)
{
    g_cursor_settings.colors.body_color = body;
    g_cursor_settings.colors.shadow_color = shadow;
}

void Cursor_SetAcceleration(int accel)
{
    if (accel < 0) accel = 0;
    if (accel > 100) accel = 100;
    g_cursor_settings.acceleration = accel;
}

void Cursor_SetDoublePixel(int enable)
{
    g_cursor_settings.double_pixel = enable ? 1 : 0;
}

CursorSettings Cursor_GetSettings(void)
{
    return g_cursor_settings;
}

void Cursor_ApplySettings(void)
{
    /* Hide cursor first to restore old background completely */
    if (cur_drawn) {
        cursor_restore_bg(cur_x, cur_y);
        cur_drawn = 0;
    }

    /* Clear background save buffer to prevent artifacts from size changes */
    for (int i = 0; i < CUR_MAX_W * CUR_MAX_H; i++) {
        bg_save[i] = 0;
    }

    /* Save new background and draw cursor with new settings */
    cursor_save_bg(cur_x, cur_y);
    cursor_draw(cur_x, cur_y);
    cur_drawn = 1;
}

/* =========================================================================
 * Custom sprite and busy cursor support
 * ========================================================================= */

void Cursor_SetCustomSprite(const uint8_t *data, int w, int h, int xoff, int yoff)
{
    if (!data || w <= 0 || h <= 0) return;
    if (w > CUR_MAX_W) w = CUR_MAX_W;
    if (h > CUR_MAX_H) h = CUR_MAX_H;

    if (cur_drawn) {
        cursor_restore_bg(cur_x, cur_y);
        cur_drawn = 0;
    }

    memset(cur_custom, 0, sizeof(cur_custom));
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            cur_custom[row * w + col] = data[row * w + col];
        }
    }
    cur_custom_w = w;
    cur_custom_h = h;
    cur_custom_x = xoff;
    cur_custom_y = yoff;
    cur_custom_active = 1;
    cur_busy = 0;

    cursor_save_bg(cur_x, cur_y);
    cursor_draw(cur_x, cur_y);
    cur_drawn = 1;
}

void Cursor_ClearCustomSprite(void)
{
    if (cur_drawn) {
        cursor_restore_bg(cur_x, cur_y);
        cur_drawn = 0;
    }

    cur_custom_active = 0;
    cur_custom_w = 0;
    cur_custom_h = 0;
    cur_custom_x = 0;
    cur_custom_y = 0;

    cursor_save_bg(cur_x, cur_y);
    cursor_draw(cur_x, cur_y);
    cur_drawn = 1;
}

void Cursor_SetBusy(int busy)
{
    if (!!cur_busy == !!busy) return;

    if (cur_drawn) {
        cursor_restore_bg(cur_x, cur_y);
        cur_drawn = 0;
    }

    cur_busy = busy ? 1 : 0;
    if (cur_busy)
        cur_custom_active = 0;

    cursor_save_bg(cur_x, cur_y);
    cursor_draw(cur_x, cur_y);
    cur_drawn = 1;
}
