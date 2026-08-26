/*
 * icon_render.c — Icon Blitting to Linear Framebuffer
 *
 * Draws ParsedIcon ARGB bitmaps directly into the UAOS back-buffer.
 */

#include "icon_render.h"
#include "framebuffer.h"
#include <stdint.h>

/* =========================================================================
 * Public
 * ========================================================================= */

void Icon_Draw(const ParsedIcon *icon, int x, int y)
{
    if (!icon || icon->image.width == 0 || icon->image.height == 0) return;

    uint16_t w = icon->image.width;
    uint16_t h = icon->image.height;

    /* P1: use FB_BlitARGB row blit instead of per-pixel FB_PutPixel. */
    for (uint16_t row = 0; row < h; row++) {
        FB_BlitARGB(x, y + row, w,
                    &icon->image.normal[row * ICON_MAX_WIDTH], 0);
    }
}

void Icon_DrawSelected(const ParsedIcon *icon, int x, int y)
{
    if (!icon || icon->image.width == 0 || icon->image.height == 0) return;

    uint16_t w = icon->image.width;
    uint16_t h = icon->image.height;

    if (icon->image.has_selected) {
        /* Use the selected planar image embedded in the .info file */
        for (uint16_t row = 0; row < h; row++) {
            FB_BlitARGB(x, y + row, w,
                        &icon->image.selected[row * ICON_MAX_WIDTH], 0);
        }
    } else {
        /* No selected image: draw the normal icon with inverse/video colors. */
        for (uint16_t row = 0; row < h; row++) {
            FB_BlitARGB(x, y + row, w,
                        &icon->image.normal[row * ICON_MAX_WIDTH], 1);
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
