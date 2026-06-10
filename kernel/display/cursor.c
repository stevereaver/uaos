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

/* 32x32 scaled arrow pointer (2x scale of 16x16) */
static const uint8_t cur_32x32[32][32] = {
/*       0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 */
/* r0 */ W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r1 */ W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r2 */ W, W, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r3 */ W, W, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r4 */ W, W, W, W, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r5 */ W, W, W, W, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r6 */ W, W, W, W, W, W, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r7 */ W, W, W, W, W, W, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r8 */ W, W, W, W, W, W, W, W, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r9 */ W, W, W, W, W, W, W, W, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r10 */ W, W, W, W, W, W, W, W, W, W, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r11 */ W, W, W, W, W, W, W, W, W, W, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r12 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r13 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r14 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _,
/*r15 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _,
/*r16 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, _, _, _, _, _, _, _, _, _, _,
/*r17 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, _, _, _, _, _, _, _, _, _, _,
/*r18 */ W, W, W, W, W, W, W, W, W, W, W, W, B, B, B, B, B, B, B, B, _, _, _, _, _, _, _, _, _, _,
/*r19 */ W, W, W, W, W, W, W, W, W, W, W, W, B, B, B, B, B, B, B, B, _, _, _, _, _, _, _, _, _, _,
/*r20 */ W, W, W, W, W, W, W, W, B, B, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r21 */ W, W, W, W, W, W, W, W, B, B, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r22 */ W, W, W, W, W, W, B, B, _, B, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r23 */ W, W, W, W, W, W, B, B, _, B, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r24 */ W, W, W, W, B, B, _, _, _, _, B, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r25 */ W, W, W, W, B, B, _, _, _, _, B, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r26 */ W, W, B, B, _, _, _, _, _, _, B, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r27 */ W, W, B, B, _, _, _, _, _, _, B, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r28 */ B, B, _, _, _, _, _, _, _, _, _, B, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r29 */ B, B, _, _, _, _, _, _, _, _, _, B, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r30 */ _, _, _, _, _, _, _, _, _, _, _, _, B, W, W, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r31 */ _, _, _, _, _, _, _, _, _, _, _, _, B, B, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _,
};

/* 48x48 scaled arrow pointer (3x scale of 16x16) */
static const uint8_t cur_48x48[48][48] = {
/*       0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 */
/* r0 */ W, W, W, B, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r1 */ W, W, W, B, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r2 */ W, W, W, B, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r3 */ W, W, W, W, W, W, B, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r4 */ W, W, W, W, W, W, B, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r5 */ W, W, W, W, W, W, B, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r6 */ W, W, W, W, W, W, W, W, W, B, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r7 */ W, W, W, W, W, W, W, W, W, B, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r8 */ W, W, W, W, W, W, W, W, W, B, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/* r9 */ W, W, W, W, W, W, W, W, W, W, W, W, B, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r10 */ W, W, W, W, W, W, W, W, W, W, W, W, B, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r11 */ W, W, W, W, W, W, W, W, W, W, W, W, B, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r12 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r13 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r14 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
/*r15 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B, _, _, _, _, _, _, _, _, _, _,
/*r16 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B, _, _, _, _, _, _, _, _, _,
/*r17 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B, _, _, _, _, _, _, _, _,
/*r18 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B, _, _, _, _,
/*r19 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B, _, _, _,
/*r20 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B, _, _,
/*r21 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r22 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r23 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r24 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r25 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r26 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r27 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r28 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r29 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r30 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r31 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r32 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r33 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r34 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r35 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r36 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r37 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r38 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r39 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r40 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r41 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r42 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r43 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r44 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r45 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r46 */ W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, W, B, B, B,
/*r47 */ _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
};


#undef _
#undef B
#undef W

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
    switch (g_cursor_settings.size) {
        case CURSOR_SIZE_16x16: return 16;
        case CURSOR_SIZE_32x32: return 32;
        case CURSOR_SIZE_48x48: return 48;
        default: return 16;
    }
}

static const uint8_t* get_cursor_sprite(void)
{
    switch (g_cursor_settings.size) {
        case CURSOR_SIZE_16x16: return (const uint8_t*)cur_16x16;
        case CURSOR_SIZE_32x32: return (const uint8_t*)cur_32x32;
        case CURSOR_SIZE_48x48: return (const uint8_t*)cur_48x48;
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
    int cur_size = get_cursor_size();

    for (int row = 0; row < cur_size; row++) {
        int py = y + row;
        for (int col = 0; col < cur_size; col++) {
            int px = x + col;
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
    int cur_size = get_cursor_size();
    
    for (int row = 0; row < cur_size; row++) {
        int py = y + row;
        if (py < 0 || py >= H) continue;
        for (int col = 0; col < cur_size; col++) {
            int px = x + col;
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
    int cur_size = get_cursor_size();
    const uint8_t *sprite = get_cursor_sprite();
    uint32_t body_col = g_cursor_settings.colors.body_color;
    uint32_t shadow_col = g_cursor_settings.colors.shadow_color;
    int double_pixel = g_cursor_settings.double_pixel;

    for (int row = 0; row < cur_size; row++) {
        int py = y + row;
        if (py < 0 || py >= H) continue;
        for (int col = 0; col < cur_size; col++) {
            int px = x + col;
            if (px < 0 || px >= W) continue;
            uint8_t p = sprite[row * cur_size + col];
            
            if (p == 1) {
                FB_PutPixel(px, py, shadow_col);
                if (double_pixel && col + 1 < cur_size && px + 1 < W)
                    FB_PutPixel(px + 1, py, shadow_col);
            } else if (p == 2) {
                FB_PutPixel(px, py, body_col);
                if (double_pixel && col + 1 < cur_size && px + 1 < W)
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

void Cursor_Move(int x, int y)
{
    if (!g_fb.valid) return;
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
