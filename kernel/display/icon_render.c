/*
 * icon_render.c — Icon Blitting to Linear Framebuffer
 *
 * Draws ParsedIcon ARGB bitmaps directly into the UAOS back-buffer.
 */

#include "icon_render.h"
#include "framebuffer.h"
#include <stdint.h>

/* =========================================================================
 * Blit one ARGB pixel with simple alpha (0 = skip, >0 = opaque)
 * ========================================================================= */

static inline void blit_pixel(int sx, int sy, uint32_t argb)
{
    if ((argb >> 24) == 0) return;          /* fully transparent */
    FB_PutPixel(sx, sy, argb & 0x00FFFFFF);
}

/* =========================================================================
 * Public
 * ========================================================================= */

void Icon_Draw(const ParsedIcon *icon, int x, int y)
{
    if (!icon || icon->image.width == 0 || icon->image.height == 0) return;

    uint16_t w = icon->image.width;
    uint16_t h = icon->image.height;

    for (uint16_t row = 0; row < h; row++) {
        for (uint16_t col = 0; col < w; col++) {
            uint32_t px = icon->image.normal[row * ICON_MAX_WIDTH + col];
            if ((px >> 24) != 0) {
                FB_PutPixel(x + col, y + row, px & 0x00FFFFFF);
            }
        }
    }
}

void Icon_DrawSelected(const ParsedIcon *icon, int x, int y)
{
    if (!icon || icon->image.width == 0 || icon->image.height == 0) return;

    uint16_t w = icon->image.width;
    uint16_t h = icon->image.height;

    for (uint16_t row = 0; row < h; row++) {
        for (uint16_t col = 0; col < w; col++) {
            uint32_t px = icon->image.selected[row * ICON_MAX_WIDTH + col];
            if ((px >> 24) != 0) {
                FB_PutPixel(x + col, y + row, px & 0x00FFFFFF);
            }
        }
    }
}

void Icon_DrawLabel(const ParsedIcon *icon, int x, int y, int icon_w)
{
    if (!icon || icon->label[0] == '\0') return;

    /* Count visible characters */
    int len = 0;
    while (icon->label[len] && len < ICON_MAX_LABEL) len++;

    int text_w = len * FB_CharWidth();
    int lx = x + (icon_w - text_w) / 2;
    if (lx < 0) lx = 0;

    /* Background highlight behind label */
    int lh = FB_CharHeight();
    FB_FillRect(lx - 2, y, text_w + 4, lh, WB_BLUE);

    FB_PutStr(lx, y, icon->label, WB_WHITE, WB_BLUE);
}
