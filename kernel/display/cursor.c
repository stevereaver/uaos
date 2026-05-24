/* cursor.c — UAOS Framebuffer Software Cursor
 *
 * Implements a 16×16 Amiga-style arrow cursor using a two-pass technique:
 *   1. Save the pixels currently under the cursor into a background buffer
 *   2. Draw the cursor sprite (AND mask + XOR mask — classic Amiga sprite)
 *
 * On move: restore saved background, save new background, draw at new pos.
 *
 * The cursor is defined in two 16×16 bitmaps:
 *   and_mask — pixels to AND with background (0 = opaque, 1 = transparent)
 *   xor_mask — pixels to XOR after AND       (sets colour of opaque pixels)
 *
 * Resulting colour per pixel:
 *   AND=0, XOR=0 → black pixel
 *   AND=0, XOR=1 → white pixel  (used for cursor body outline)
 *   AND=1, XOR=0 → background unchanged (transparent)
 *   AND=1, XOR=1 → background inverted  (not used here)
 */

#include "cursor.h"
#include "framebuffer.h"
#include <stdint.h>

/* =========================================================================
 * Cursor sprite — 16×16 Amiga-style arrow
 * Each row is a 16-bit mask, MSB = left pixel.
 * ========================================================================= */

#define CUR_W  16
#define CUR_H  16

/*
 * Authentic Amiga Workbench 3.x arrow pointer.
 * Pixel map: 0=transparent, 1=black (shadow/outline), 2=white (body)
 * Hotspot at (0,0). White body with black drop-shadow on right/bottom edges.
 *
 *  W = white body pixel
 *  B = black shadow/outline pixel
 *  . = transparent
 *
 *  Col: 0  1  2  3  4  5  6  7  8  9 ...
 *  r0:  W  .  .  .  .  .  .  .  .  .
 *  r1:  W  W  .  .  .  .  .  .  .  .
 *  r2:  W  W  W  .  .  .  .  .  .  .
 *  r3:  W  W  W  W  .  .  .  .  .  .
 *  r4:  W  W  W  W  W  .  .  .  .  .
 *  r5:  W  W  W  W  W  W  .  .  .  .
 *  r6:  W  W  W  W  W  W  W  .  .  .
 *  r7:  W  W  W  W  W  W  W  W  .  .
 *  r8:  W  W  W  W  W  W  W  W  W  .
 *  r9:  W  W  W  W  W  W  B  B  B  .
 *  r10: W  W  W  B  W  W  .  .  .  .
 *  r11: W  W  B  .  W  W  .  .  .  .
 *  r12: W  B  .  .  .  W  W  .  .  .
 *  r13: B  .  .  .  .  .  W  W  .  .
 *  r14: .  .  .  .  .  .  .  W  B  .
 *  r15: .  .  .  .  .  .  .  .  .  .
 */
#define _ 0
#define B 1
#define W 2

static const uint8_t cur_pixels[CUR_H][CUR_W] = {
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

#undef _
#undef B
#undef W

/* =========================================================================
 * Background save buffer
 * ========================================================================= */

static uint32_t bg_save[CUR_W * CUR_H];
static int      cur_x = 0;
static int      cur_y = 0;
static int      cur_drawn = 0;   /* 1 if cursor is currently on screen */

/* =========================================================================
 * Background save / restore
 * ========================================================================= */

static void cursor_save_bg(int x, int y)
{
    uint8_t *base  = (uint8_t *)(uintptr_t)g_fb.phys_addr;
    uint32_t pitch = g_fb.pitch;
    uint8_t  bpp   = g_fb.bpp;
    int      W     = (int)g_fb.width;
    int      H     = (int)g_fb.height;

    for (int row = 0; row < CUR_H; row++) {
        int py = y + row;
        if (py < 0 || py >= H) {
            for (int col = 0; col < CUR_W; col++)
                bg_save[row * CUR_W + col] = 0;
            continue;
        }
        for (int col = 0; col < CUR_W; col++) {
            int px = x + col;
            if (px < 0 || px >= W) {
                bg_save[row * CUR_W + col] = 0;
                continue;
            }
            if (bpp == 32) {
                uint32_t *p = (uint32_t *)(base + (uint32_t)py * pitch + (uint32_t)px * 4);
                bg_save[row * CUR_W + col] = *p;
            } else {
                uint8_t *p = base + (uint32_t)py * pitch + (uint32_t)px * 3;
                bg_save[row * CUR_W + col] =
                    (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
            }
        }
    }
}

static void cursor_restore_bg(int x, int y)
{
    int W = (int)g_fb.width;
    int H = (int)g_fb.height;
    for (int row = 0; row < CUR_H; row++) {
        int py = y + row;
        if (py < 0 || py >= H) continue;
        for (int col = 0; col < CUR_W; col++) {
            int px = x + col;
            if (px < 0 || px >= W) continue;
            FB_PutPixel(px, py, bg_save[row * CUR_W + col]);
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
    for (int row = 0; row < CUR_H; row++) {
        int py = y + row;
        if (py < 0 || py >= H) continue;
        for (int col = 0; col < CUR_W; col++) {
            int px = x + col;
            if (px < 0 || px >= W) continue;
            uint8_t p = cur_pixels[row][col];
            if (p == 1)      FB_PutPixel(px, py, WB_BLACK);
            else if (p == 2) FB_PutPixel(px, py, WB_WHITE);
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
    if (cur_drawn) {
        cursor_restore_bg(cur_x, cur_y);
        cur_drawn = 0;
    }
}
