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
#include "../irq/irq.h"
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
static volatile int cur_x = 0;  /* requested position (written at IRQ time) */
static volatile int cur_y = 0;
static int      drw_x = 0;    /* position the sprite is painted at (visible buffer) */
static int      drw_y = 0;
static int      bb_x = 0;     /* pos the sprite was last painted INTO THE BACK BUFFER */
static int      bb_y = 0;
static int      bb_drawn = 0;    /* 1 if sprite pixels may linger in the back buffer */
static int      cur_drawn = 0;   /* 1 if cursor is currently on screen */
static volatile int cur_moved = 0; /* IRQ-deferred move pending (UAOS-104) */

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

    /* Prefer the back buffer: it mirrors every write, so once the WM has
     * run its first full flip the save is a plain RAM copy.  VRAM reads
     * are a bus transaction each on write-combining / uncached apertures
     * (the MBP4,1 G84 boots with nomtrr), which made mouse motion visibly
     * stall the pointer. */
    int use_bb = FB_IsDrawing() || FB_BackbufCoherent();

    for (int row = 0; row < cur_h; row++) {
        uint32_t *dst = &bg_save[row * CUR_MAX_W];
        int py = y + off_y + row;
        int sx = x + off_x;
        int c0 = 0, c1 = cur_w;
        if (py < 0 || py >= H) {
            c1 = 0;
        } else {
            if (sx < 0) c0 = -sx;
            if (c0 > cur_w) c0 = cur_w;
            if (sx + cur_w > W) c1 = W - sx;
            if (c1 < 0) c1 = 0;
        }
        for (int c = 0; c < c0; c++) dst[c] = 0;
        for (int c = c1; c < cur_w; c++) dst[c] = 0;
        if (c0 >= c1) continue;

        const uint32_t *brow = use_bb ? FB_BackbufRow(py) : NULL;
        if (brow) {
            memcpy(dst + c0, brow + sx + c0, (size_t)(c1 - c0) * 4);
        } else if (g_fb.bpp == 32) {
            memcpy(dst + c0,
                   (const uint8_t *)(uintptr_t)g_fb.phys_addr
                       + (uint32_t)py * g_fb.pitch + (uint32_t)(sx + c0) * 4,
                   (size_t)(c1 - c0) * 4);
        } else {
            for (int c = c0; c < c1; c++)
                dst[c] = FB_GetPixel(sx + c, py);
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

    /* Fast path: 32bpp row memcpy back to VRAM, mirrored into the back
     * buffer so the shadow stays coherent for the next save. */
    if (g_fb.bpp == 32) {
        uint8_t *base = (uint8_t *)(uintptr_t)g_fb.phys_addr;
        for (int row = 0; row < cur_h; row++) {
            int py = y + off_y + row;
            if (py < 0 || py >= H) continue;
            int sx = x + off_x;
            int c0 = 0, c1 = cur_w;
            if (sx < 0) c0 = -sx;
            if (c0 > cur_w) c0 = cur_w;
            if (sx + cur_w > W) c1 = W - sx;
            if (c1 > cur_w) c1 = cur_w;
            if (c0 >= c1) continue;
            memcpy(base + (uint32_t)py * g_fb.pitch + (uint32_t)(sx + c0) * 4,
                   &bg_save[row * CUR_MAX_W + c0],
                   (size_t)(c1 - c0) * 4);
            uint32_t *brow = FB_BackbufRow(py);
            if (brow)
                memcpy(brow + sx + c0, &bg_save[row * CUR_MAX_W + c0],
                       (size_t)(c1 - c0) * 4);
        }
        return;
    }

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
    int H = (int)g_fb.height;
    int cur_h = get_cursor_size();
    int cur_w = get_cursor_width();
    const uint8_t *sprite = get_cursor_sprite();
    uint32_t body_col = g_cursor_settings.colors.body_color;
    uint32_t shadow_col = g_cursor_settings.colors.shadow_color;
    int double_pixel = g_cursor_settings.double_pixel;
    int off_x = cur_custom_active ? cur_custom_x : 0;
    int off_y = cur_custom_active ? cur_custom_y : 0;

    /* One ARGB scanline per sprite row: a single FB_BlitARGB call instead
     * of a function-called FB_PutPixel per pixel (~230 calls for 16x16
     * down to ~16 row blits).  Alpha 0 keeps the background transparent. */
    uint32_t argb[CUR_MAX_W];
    for (int row = 0; row < cur_h; row++) {
        int py = y + off_y + row;
        if (py < 0 || py >= H) continue;
        const uint8_t *srow = sprite + row * cur_w;
        memset(argb, 0, (size_t)cur_w * sizeof(argb[0]));
        for (int col = 0; col < cur_w; col++) {
            uint8_t p = srow[col];
            uint32_t c;
            if (p == 1)      c = 0xFF000000u | shadow_col;
            else if (p == 2) c = 0xFF000000u | body_col;
            else             continue;   /* transparent — keep 0 (or a
                                          * doubled pixel from the left) */
            argb[col] = c;
            if (double_pixel && col + 1 < cur_w)
                argb[col + 1] = c;   /* later sprite pixel overwrites */
        }
        FB_BlitARGB(x + off_x, py, cur_w, argb, 0);
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/* Paint the sprite at the requested position and record where it landed.
 * Clears the deferred-move flag: any move serviced by this paint is done. */
static void cursor_commit_draw(void)
{
    /* Snapshot the requested position once, atomically.  IRQ-side
     * Cursor_Move can retarget mid-commit; without a single snapshot the
     * background save, sprite draw and drw_x/drw_y record could each see
     * a different position, so the next restore would stamp stale pixels
     * at the wrong spot — the stray-fragment glitch in UAOS-189.  Clearing
     * cur_moved inside the snapshot means a move that lands during the
     * paint stays pending for the next flush. */
    uint64_t flags = irq_save();
    int x = cur_x;
    int y = cur_y;
    cur_moved = 0;
    irq_restore(flags);

    cursor_save_bg(x, y);
    cursor_draw(x, y);
    drw_x = x;
    drw_y = y;
    cur_drawn = 1;
    /* While a back-buffered frame is in progress the sprite lands in the
     * back buffer too — record that position so the next damage-scoped
     * repaint can erase it (otherwise it would linger and ghost once the
     * cursor moves away). */
    if (FB_IsDrawing()) {
        bb_x = x;
        bb_y = y;
        bb_drawn = 1;
    }
}

void Cursor_Init(int x, int y)
{
    cur_x     = x;
    cur_y     = y;
    drw_x     = x;
    drw_y     = y;
    cur_drawn = 0;
    cur_moved = 0;
    if (!g_fb.valid) return;
    cursor_commit_draw();
}

/* Intuition may have scheduled a delayed pointer change via WA_PointerDelay. */
extern void UAOS_Intuition_CheckPendingPointer(void);

void Cursor_Move(int x, int y)
{
    /* UAOS-104: called from PS2Mouse_IRQHandler once per mouse packet.
     * Only record the target position here — the save/restore/draw runs
     * once per frame from Cursor_Flush() (idle loop) or the frame-end
     * Cursor_Redraw() inside a back-buffered WM repaint, so a burst of
     * packets coalesces into a single paint instead of three per-pixel
     * passes per packet at IRQ time. */
    cur_x = x;
    cur_y = y;
    cur_moved = 1;
}

void Cursor_Flush(void)
{
    if (!cur_moved) return;
    cur_moved = 0;
    UAOS_Intuition_CheckPendingPointer();
    /* While a back-buffered frame is in progress the frame-end
     * Cursor_Redraw paints the cursor at the new position anyway. */
    if (!g_fb.valid || FB_IsDrawing()) return;
    /* Early-out: sprite already painted at the target position. */
    if (cur_drawn && cur_x == drw_x && cur_y == drw_y) return;
    if (cur_drawn)
        cursor_restore_bg(drw_x, drw_y);
    cursor_commit_draw();
}

void Cursor_Redraw(void)
{
    UAOS_Intuition_CheckPendingPointer();
    if (!g_fb.valid) return;
    if (cur_drawn)
        cursor_restore_bg(drw_x, drw_y);
    cursor_commit_draw();
}

void Cursor_Hide(void)
{
    if (!g_fb.valid) return;
    if (FB_IsDrawing()) return;  /* no-op during double-buffered draw */
    if (cur_drawn) {
        cursor_restore_bg(drw_x, drw_y);
        cur_drawn = 0;
    }
}

/* Union of the sprite footprint in the visible buffer (drw pos) and in the
 * back buffer (bb pos).  The WM damages this before a damage-scoped repaint
 * so the scene repaint erases stale sprite pixels from BOTH buffers before
 * cursor_save_bg() samples the new position — prevents ghosting and stops
 * the save buffer capturing old sprite pixels as "background" (UAOS-104). */
int Cursor_GetSpriteRect(int *x, int *y, int *w, int *h)
{
    int cw = get_cursor_width();
    int ch = get_cursor_size();
    int ox = cur_custom_active ? cur_custom_x : 0;
    int oy = cur_custom_active ? cur_custom_y : 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, have = 0;

    if (cur_drawn) {
        x0 = drw_x + ox;  y0 = drw_y + oy;
        x1 = x0 + cw;     y1 = y0 + ch;
        have = 1;
    }
    if (bb_drawn) {
        int bx0 = bb_x + ox, by0 = bb_y + oy;
        if (!have) {
            x0 = bx0; y0 = by0; x1 = bx0 + cw; y1 = by0 + ch;
            have = 1;
        } else {
            if (bx0 < x0) x0 = bx0;
            if (by0 < y0) y0 = by0;
            if (bx0 + cw > x1) x1 = bx0 + cw;
            if (by0 + ch > y1) y1 = by0 + ch;
        }
    }
    if (!have) return 0;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return 1;
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
        cursor_restore_bg(drw_x, drw_y);
        cur_drawn = 0;
    }

    /* Clear background save buffer to prevent artifacts from size changes */
    for (int i = 0; i < CUR_MAX_W * CUR_MAX_H; i++) {
        bg_save[i] = 0;
    }

    /* Save new background and draw cursor with new settings */
    cursor_commit_draw();
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
        cursor_restore_bg(drw_x, drw_y);
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

    cursor_commit_draw();
}

void Cursor_ClearCustomSprite(void)
{
    if (cur_drawn) {
        cursor_restore_bg(drw_x, drw_y);
        cur_drawn = 0;
    }

    cur_custom_active = 0;
    cur_custom_w = 0;
    cur_custom_h = 0;
    cur_custom_x = 0;
    cur_custom_y = 0;

    cursor_commit_draw();
}

void Cursor_SetBusy(int busy)
{
    if (!!cur_busy == !!busy) return;

    if (cur_drawn) {
        cursor_restore_bg(drw_x, drw_y);
        cur_drawn = 0;
    }

    cur_busy = busy ? 1 : 0;
    if (cur_busy)
        cur_custom_active = 0;

    cursor_commit_draw();
}
