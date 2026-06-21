/*
 * icon_def.h — Amiga Workbench .info Icon Format Definitions
 *
 * Structures for AmigaOS DiskObject / icon imagery.
 * Compatible with classic 1.x and 2.0+ .info formats.
 */

#ifndef UAOS_ICON_DEF_H
#define UAOS_ICON_DEF_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Version / type constants
 * ------------------------------------------------------------------------- */
#define WB_DISKVERSION      1
#define WB_DISKREVISION     1
#define WB_IMAGETYPE_PLANAR 0   /* Classic bitplane icon */
#define WB_IMAGETYPE_NEW    1   /* NewIcons (compressed, unsupported yet) */
#define WB_IMAGETYPE_PNG    2   /* OS 3.5+ PNG (unsupported yet) */

/* Gadget types used inside DiskObject.gadget */
#define GTYP_CUSTOM   0x0001
#define GTYP_GADGET   0x0002

/* Icon types (DiskObject.do_Type) */
#define WB_DISK       1   /* Hard disk volume */
#define WB_DRAWER     2   /* Directory / drawer */
#define WB_TOOL       3   /* Executable tool */
#define WB_PROJECT    4   /* Project file (data) */
#define WB_GARBAGE    5   /* Trashcan */
#define WB_DEVICE     6   /* Device */
#define WB_KICK       7   /* Kickstart */
#define WB_APPICON    8   /* App-managed icon */

/* -------------------------------------------------------------------------
 * Image structure — one bitplane pair (normal or selected)
 * ------------------------------------------------------------------------- */
typedef struct {
    uint16_t Width;
    uint16_t Height;
    uint16_t Depth;        /* bitplanes (usually 2 for 4 colours) */
    uint16_t ImageDataSize;/* bytes per plane */
    uint32_t *PlanePick;
    uint32_t *PlaneOnOff;
    uint32_t *ImageData;   /* planar bitplane data (guest pointer) */
} WBImage;

/* -------------------------------------------------------------------------
 * Gadget structure — embedded in DiskObject, defines icon hit box
 * ------------------------------------------------------------------------- */
typedef struct {
    uint16_t GadgetType;
    uint16_t GadgetRenderFlags;
    uint32_t GadgetRender; /* pointer to WBImage (guest) */
    uint32_t SelectRender; /* pointer to WBImage (guest) */
    int16_t  LeftEdge;
    int16_t  TopEdge;
    uint16_t Width;
    uint16_t Height;
} WBGadget;

/* -------------------------------------------------------------------------
 * DiskObject — main .info file header
 * ------------------------------------------------------------------------- */
typedef struct {
    uint16_t do_Magic;          /* 0xE310 (wbobject) */
    uint16_t do_Version;        /* should be 1 */
    uint32_t do_Gadget;         /* pointer to WBGadget (guest) */
    uint8_t  do_Type;
    uint8_t  do_Pad;
    uint32_t do_DefaultTool;    /* guest pointer to default tool string */
    uint32_t do_ToolTypes;      /* guest pointer to array of string pointers */
    int16_t  do_CurrentX;       /* icon position on desktop */
    int16_t  do_CurrentY;
    uint32_t do_DrawerData;     /* guest pointer (DrawerData, not used yet) */
    uint32_t do_ToolWindow;     /* guest pointer to tool window name */
    int16_t  do_StackSize;      /* stack size for tools */
} DiskObject;

#define WB_DISKOBJECT_MAGIC 0xE310

/* -------------------------------------------------------------------------
 * Parsed (native) icon representation used by kernel / display code
 * ------------------------------------------------------------------------- */
#define ICON_MAX_PLANES  8
#define ICON_MAX_WIDTH   32
#define ICON_MAX_HEIGHT  32
#define ICON_MAX_LABEL   32
#define ICON_MAX_TOOLTYPE_LEN 128
#define ICON_MAX_TOOLTYPES    16

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t depth;
    uint8_t  has_selected;                               /* 1 if selected image present in .info */
    uint8_t  pad;
    uint32_t normal[ICON_MAX_WIDTH * ICON_MAX_HEIGHT];   /* ARGB */
    uint32_t selected[ICON_MAX_WIDTH * ICON_MAX_HEIGHT]; /* ARGB */
} IconImage;

typedef struct {
    char     label[ICON_MAX_LABEL];
    uint8_t  type;                      /* WB_DISK, WB_DRAWER, etc. */
    int16_t  pos_x;
    int16_t  pos_y;
    IconImage image;
    char     default_tool[ICON_MAX_LABEL];
    char     tool_types[ICON_MAX_TOOLTYPES][ICON_MAX_TOOLTYPE_LEN];
    int      tool_type_count;
} ParsedIcon;

#endif /* UAOS_ICON_DEF_H */
