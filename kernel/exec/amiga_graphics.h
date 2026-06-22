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
#define RP_OFF_SOFTSTYLE    44   /* soft font style (JAM1/JAM2 etc.) */
#define RP_OFF_MASK         40   /* write mask (V39) */
#define RP_OFF_MAXPEN       41   /* max pen used in RastPort (V39) */
#define RP_SIZE_MIN         64   /* enough to cover fields we touch */

/* BitMap flags for AllocBitMap / GetBitMapAttr */
#define BMF_CLEAR        0x00000001
#define BMF_DISPLAYABLE  0x00000002
#define BMF_INTERLEAVED  0x00000004
#define BMF_STANDARD     0x00000008
#define BMF_MINPLANES    0x00000010

/* TextFont structure offsets */
#define TF_OFF_NODE          0
#define TF_OFF_YSIZE        14
#define TF_OFF_STYLE        16
#define TF_OFF_FLAGS        17
#define TF_OFF_XSIZE        18
#define TF_OFF_BASELINE     20
#define TF_OFF_BOLDSMEAR    22
#define TF_OFF_ACCESSORS    24
#define TF_OFF_LOCHAR       26
#define TF_OFF_HICHAR       27
#define TF_OFF_CHARDATA     28
#define TF_OFF_MODULO       32
#define TF_OFF_CHARSPACE    34
#define TF_OFF_CHARKERN     38
#define TF_SIZE             42

/* TextAttr structure offsets */
#define TA_OFF_NAME          0
#define TA_OFF_YSIZE         4
#define TA_OFF_STYLE         6
#define TA_OFF_FLAGS         7
#define TA_SIZE              8

/* TextExtent structure offsets */
#define TE_OFF_EXTENT_X      0
#define TE_OFF_EXTENT_Y      2
#define TE_OFF_EXTENT_WIDTH  4
#define TE_OFF_EXTENT_HEIGHT 6
#define TE_OFF_WIDTH         8
#define TE_OFF_HEIGHT       10
#define TE_SIZE             12

/* Font style flags */
#define FSF_BOLD        0x01
#define FSF_ITALIC      0x02
#define FSF_UNDERLINE   0x04
#define FSF_EXTENDED    0x08

/* Rectangle offsets */
#define RECT_MINX            0
#define RECT_MINY            2
#define RECT_MAXX            4
#define RECT_MAXY            6
#define RECT_SIZE            8

/* Region offsets */
#define RG_OFF_REGIONRECT    0
#define RG_OFF_MINX          4
#define RG_OFF_MINY          6
#define RG_OFF_MAXX          8
#define RG_OFF_MAXY         10
#define RG_SIZE             12

/* RegionRectangle offsets */
#define RR_OFF_NEXT          0
#define RR_OFF_PREV          4
#define RR_OFF_MINX          8
#define RR_OFF_MINY          10
#define RR_OFF_MAXX          12
#define RR_OFF_MAXY          14
#define RR_SIZE             16

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

/* View offsets */
#define VIEW_OFF_VIEWPORT    0
#define VIEW_OFF_DX          8
#define VIEW_OFF_DY          10
#define VIEW_OFF_FLAGS       12

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

/* RasInfo offsets */
#define RI_OFF_BITMAP        0
#define RI_OFF_NEXT          4
#define RI_OFF_RXOFFSET      8
#define RI_OFF_RYOFFSET      10

/* ColorMap offsets */
#define CM_OFF_TYPE          0
#define CM_OFF_FLAGS         1
#define CM_OFF_COUNT         2
#define CM_OFF_TABLEENTRIES  4
#define CM_OFF_COLORTABLE    6
#define CM_OFF_PALEXTRA     10

/* UAOS ColorMap extension fields (private, after AmigaOS public layout) */
#define CM_OFF_VP           14
#define CM_OFF_VPE          18
#define CM_OFF_NORMAL_DISP  22
#define CM_OFF_COERCE_DISP  26
#define CM_OFF_VPMODEID     30
#define CM_OFF_BORDERBLANK  34
#define CM_OFF_CHROMAKEY    35
#define CM_OFF_BITPLANEKEY  36
#define CM_OFF_CHROMA_PEN   37
#define CM_OFF_USERCLIP     38
#define CM_OFF_PF1_BASE     39
#define CM_OFF_PF2_BASE     40
#define CM_OFF_SPEVEN_BASE  41
#define CM_OFF_SPODD_BASE   42
#define CM_SIZE             43

/* VideoControl tag values (AmigaOS 3.x) */
#define VTAG_END_CM                     0x00000000
#define VTAG_CHROMAKEY_CLR              0x80000000
#define VTAG_CHROMAKEY_SET              0x80000001
#define VTAG_BITPLANEKEY_CLR            0x80000002
#define VTAG_BITPLANEKEY_SET            0x80000003
#define VTAG_BORDERBLANK_CLR            0x80000004
#define VTAG_BORDERBLANK_SET            0x80000005
#define VTAG_BORDERNOTRANS_CLR          0x80000006
#define VTAG_BORDERNOTRANS_SET          0x80000007
#define VTAG_CHROMA_PEN_CLR             0x80000008
#define VTAG_CHROMA_PEN_SET             0x80000009
#define VTAG_CHROMA_PLANE_SET           0x8000000A
#define VTAG_ATTACH_CM_SET              0x8000000B
#define VTAG_NEXTBUF_CM                 0x8000000C
#define VTAG_BATCH_CM_CLR               0x8000000D
#define VTAG_BATCH_CM_SET               0x8000000E
#define VTAG_NORMAL_DISP_GET            0x8000000F
#define VTAG_NORMAL_DISP_SET            0x80000010
#define VTAG_COERCE_DISP_GET            0x80000011
#define VTAG_COERCE_DISP_SET            0x80000012
#define VTAG_VIEWPORTEXTRA_GET          0x80000013
#define VTAG_VIEWPORTEXTRA_SET          0x80000014
#define VTAG_CHROMAKEY_GET              0x80000015
#define VTAG_BITPLANEKEY_GET            0x80000016
#define VTAG_BORDERBLANK_GET            0x80000017
#define VTAG_BORDERNOTRANS_GET          0x80000018
#define VTAG_CHROMA_PEN_GET             0x80000019
#define VTAG_CHROMA_PLANE_GET           0x8000001A
#define VTAG_ATTACH_CM_GET              0x8000001B
#define VTAG_BATCH_CM_GET               0x8000001C
#define VTAG_BATCH_ITEMS_GET            0x8000001D
#define VTAG_BATCH_ITEMS_SET            0x8000001E
#define VTAG_BATCH_ITEMS_ADD            0x8000001F
#define VTAG_VPMODEID_GET               0x80000020
#define VTAG_VPMODEID_SET               0x80000021
#define VTAG_VPMODEID_CLR               0x80000022
#define VTAG_USERCLIP_GET               0x80000023
#define VTAG_USERCLIP_SET               0x80000024
#define VTAG_USERCLIP_CLR               0x80000025
#define VTAG_PF1_BASE_GET               0x80000026
#define VTAG_PF2_BASE_GET               0x80000027
#define VTAG_SPEVEN_BASE_GET            0x80000028
#define VTAG_SPODD_BASE_GET             0x80000029
#define VTAG_PF1_BASE_SET               0x8000002A
#define VTAG_PF2_BASE_SET               0x8000002B
#define VTAG_SPEVEN_BASE_SET            0x8000002C
#define VTAG_SPODD_BASE_SET             0x8000002D
#define VTAG_BORDERSPRITE_GET           0x8000002E
#define VTAG_BORDERSPRITE_SET           0x8000002F
#define VTAG_BORDERSPRITE_CLR           0x80000030
#define VTAG_SPRITERESN_SET             0x80000031
#define VTAG_SPRITERESN_GET             0x80000032
#define VTAG_PF1_TO_SPRITEPRI_SET       0x80000033
#define VTAG_PF1_TO_SPRITEPRI_GET       0x80000034
#define VTAG_PF2_TO_SPRITEPRI_SET       0x80000035
#define VTAG_PF2_TO_SPRITEPRI_GET       0x80000036

/* Drawing modes */
#define JAM1        0
#define JAM2        1
#define COMPLEMENT  2
#define INVERSVID   3
#define OUTLINE     8

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
