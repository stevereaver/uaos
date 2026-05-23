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

/* AND mask: 0 = draw cursor pixel, 1 = show background */
static const uint16_t cur_and[CUR_H] = {
    0x0000,   /* 1111111111111111  ← row 0: tip of arrow (fully opaque) */
    0x4000,   /* 0100000000000000 */
    0x6000,   /* 0110000000000000 */
    0x7000,   /* 0111000000000000 */
    0x7800,   /* 0111100000000000 */
    0x7C00,   /* 0111110000000000 */
    0x7E00,   /* 0111111000000000 */
    0x7F00,   /* 0111111100000000 */
    0x7F80,   /* 0111111110000000 */
    0x7C00,   /* 0111110000000000 */
    0x6C00,   /* 0110110000000000 */
    0x4600,   /* 0100011000000000 */
    0x0600,   /* 0000011000000000 */
    0x0300,   /* 0000001100000000 */
    0x0300,   /* 0000001100000000 */
    0x0000,   /* 0000000000000000 */
};

/* XOR mask: 0 = black, 1 = white (applied only where AND=0) */
static const uint16_t cur_xor[CUR_H] = {
    0x0000,
    0xC000,
    0xE000,
    0xF000,
    0xF800,
    0xFC00,
    0xFE00,
    0xFF00,
    0xFF80,
    0xFC00,
    0xEC00,
    0xC600,
    0x0600,
    0x0300,
    0x0300,
    0x0180,
};

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
        uint16_t and_row = cur_and[row];
        uint16_t xor_row = cur_xor[row];
        for (int col = 0; col < CUR_W; col++) {
            int px = x + col;
            if (px < 0 || px >= W) continue;
            uint16_t bit = (uint16_t)(0x8000 >> col);
            int and_bit = (and_row & bit) ? 1 : 0;
            int xor_bit = (xor_row & bit) ? 1 : 0;
            if (!and_bit) {
                /* Opaque pixel */
                uint32_t colour = xor_bit ? WB_WHITE : WB_BLACK;
                FB_PutPixel(px, py, colour);
            }
            /* and_bit=1 → transparent, leave background as-is */
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
