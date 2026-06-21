/* amiga_graphics.h — Minimal AmigaOS graphics structures
 *
 * Just enough layout to dereference guest RastPort/BitMap pointers
 * from m68k code.  Offsets match classic AmigaOS 3.x.
 */

#ifndef UAOS_AMIGA_GRAPHICS_H
#define UAOS_AMIGA_GRAPHICS_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * RastPort offsets (classic AmigaOS)
 * ------------------------------------------------------------------------- */
#define RP_OFF_LAYER        0
#define RP_OFF_BITMAP       4
#define RP_OFF_AREAPTRN     8
#define RP_OFF_TMPBUF       12
#define RP_OFF_FLAGS        16
#define RP_OFF_DRAWMODE     17
#define RP_OFF_AREACNTN     18
#define RP_OFF_AREAPNCTR    19
#define RP_OFF_LINPATN      20
#define RP_OFF_CP_X         22
#define RP_OFF_CP_Y         24
#define RP_OFF_FGPEN        26
#define RP_OFF_BGPEN        27
#define RP_OFF_AOLPEN       28
#define RP_OFF_OLNPEN       29
#define RP_OFF_FONT         30
#define RP_OFF_MASK         40   /* write mask (V39) */
#define RP_OFF_MAXPEN       41   /* max pen used in RastPort (V39) */
#define RP_SIZE_MIN         64   /* enough to cover fields we touch */

/* BitMap offsets */
#define BM_OFF_BYTESPERROW   0
#define BM_OFF_ROWS          2
#define BM_OFF_FLAGS         4
#define BM_OFF_DEPTH         5
#define BM_OFF_PLANES        8

/* BitMap attribute tags for GetBitMapAttr */
#define BMA_WIDTH            0
#define BMA_HEIGHT           1
#define BMA_DEPTH            2
#define BMA_FLAGS            3
#define BMA_BASE             4
#define BMA_ROWBYTES         5

/* ViewPort offsets */
#define VP_OFF_RASINFO      14
#define VP_OFF_COLORMAP     18
#define VP_OFF_DWIDTH       22
#define VP_OFF_DHEIGHT      24
#define VP_OFF_DXOFFSET     26
#define VP_OFF_DYOFFSET     28
#define VP_OFF_MODES        30
#define VP_OFF_SPRITE       32
#define VP_OFF_COLORSET     34
#define VP_OFF_DISPLAYID    36

/* Drawing modes */
#define JAM1        0
#define JAM2        1
#define COMPLEMENT  2
#define INVERSVID   3

/* -------------------------------------------------------------------------
 * Classic Amiga 4-bit pen → 0x00RRGGBB palette (OCS/ECS default)
 * ------------------------------------------------------------------------- */
static inline uint32_t amiga_pen_to_rgb(uint8_t pen)
{
    static const uint32_t palette[16] = {
        0x00000000, /*  0 black          */
        0x00FFFFFF, /*  1 white          */
        0x00000088, /*  2 blue           */
        0x00FFFF00, /*  3 yellow         */
        0x00008800, /*  4 green          */
        0x00880000, /*  5 red            */
        0x000088FF, /*  6 light blue     */
        0x00FF8800, /*  7 orange         */
        0x00888888, /*  8 grey           */
        0x0000FFFF, /*  9 cyan           */
        0x00FF00FF, /* 10 magenta        */
        0x0000FF88, /* 11 light green    */
        0x000000FF, /* 12 dark blue      */
        0x00880088, /* 13 purple         */
        0x00888800, /* 14 olive          */
        0x00CCCCCC, /* 15 light grey     */
    };
    if (pen < 16) return palette[pen];
    /* Pens >= 16: use grey fallback (extendable for AGA direct colour) */
    return 0x00AAAAAA;
}

#endif /* UAOS_AMIGA_GRAPHICS_H */
