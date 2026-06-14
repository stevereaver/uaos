/*
 * icon_render.h — Icon Blitting to Linear Framebuffer
 */

#ifndef UAOS_ICON_RENDER_H
#define UAOS_ICON_RENDER_H

#include "../exec/icon_def.h"

/* Draw a parsed icon (normal state) at screen coordinates (x,y).
 * Transparent pixels (alpha 0) are not drawn. */
void Icon_Draw(const ParsedIcon *icon, int x, int y);

/* Draw selected state (highlighted). */
void Icon_DrawSelected(const ParsedIcon *icon, int x, int y);

/* Draw icon label below the image, centred. */
void Icon_DrawLabel(const ParsedIcon *icon, int x, int y, int icon_w);

/* Dimensions of the on-screen icon cell */
#define ICON_CELL_W   64
#define ICON_CELL_H   72

#endif /* UAOS_ICON_RENDER_H */
