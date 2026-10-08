/*
 * icon_lib.h — icon.library + real Amiga .info format support (UAOS-253)
 *
 * Two layers live here:
 *
 *   1. A host-side parser/serialiser for the *real* classic Amiga .info
 *      format (78-byte DiskObject header with an embedded 44-byte Gadget,
 *      presence-flag pointer fields, length-prefixed strings, tooltype
 *      count block).  Used by the Workbench launcher, the native icon
 *      loader (ParsedIcon) and the guest-facing icon.library handlers.
 *
 *   2. The icon.library ROM module — GetDiskObject/PutDiskObject/
 *      FreeDiskObject/FindToolType/MatchToolValue and friends dispatched
 *      through the LIB_ROM generated-base machinery (UAOS-238).
 */

#ifndef UAOS_ICON_LIB_H
#define UAOS_ICON_LIB_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Classic .info on-disk / in-memory layout (struct DiskObject, pack(2))
 * ------------------------------------------------------------------------- */
#define ICON_MAGIC        0xE310u
#define ICON_HDR_SIZE     78u     /* DiskObject header in file AND memory   */
#define ICON_GADGET_OFF   4u      /* embedded struct Gadget (44 bytes)      */
#define ICON_GADGET_SIZE  44u
#define ICON_TYPE_OFF     48u     /* UBYTE do_Type (+pad at 49)             */
#define ICON_DEFTOOL_OFF  50u     /* presence flag / STRPTR                 */
#define ICON_TTYPES_OFF   54u     /* presence flag / STRPTR*                */
#define ICON_CURX_OFF     58u     /* LONG                                   */
#define ICON_CURY_OFF     62u     /* LONG                                   */
#define ICON_DRAWER_OFF   66u     /* presence flag / DrawerData*            */
#define ICON_TOOLWIN_OFF  70u     /* presence flag / STRPTR                 */
#define ICON_STACK_OFF    74u     /* LONG                                   */

/* struct Gadget fields inside the embedded 44-byte gadget */
#define ICON_GAD_RENDER   (ICON_GADGET_OFF + 18)  /* GadgetRender (Image*)  */
#define ICON_GAD_SELREN   (ICON_GADGET_OFF + 22)  /* SelectRender (Image*)  */
#define ICON_GAD_LEFT     (ICON_GADGET_OFF + 4)
#define ICON_GAD_TOP      (ICON_GADGET_OFF + 6)
#define ICON_GAD_WIDTH    (ICON_GADGET_OFF + 8)
#define ICON_GAD_HEIGHT   (ICON_GADGET_OFF + 10)
#define ICON_GAD_FLAGS    (ICON_GADGET_OFF + 12)
#define ICON_GAD_TYPE     (ICON_GADGET_OFF + 16)
#define ICON_GAD_USERDATA (ICON_GADGET_OFF + 40)

/* struct Image — 20-byte header + planar data (rows word-aligned, per
 * plane, depth planes in sequence). */
#define ICON_IMAGE_HDR    20u

#define ICON_DRAWERDATA_FILE 56u   /* OldDrawerData in the file */
#define ICON_DRAWERDATA_MEM  62u   /* DrawerData in memory (+flags/viewmodes) */

#define ICON_MAX_TT        24
#define ICON_MAX_TT_LEN    128
#define ICON_MAX_STR       96

/* Parsed metadata extracted from a real .info file.  Also carries file
 * offsets of the variable-length sections so consumers can copy image /
 * drawer payloads verbatim. */
typedef struct {
    uint8_t  ok;
    uint8_t  type;                        /* WB_TOOL / WB_PROJECT / ...    */
    int32_t  cur_x, cur_y;
    int32_t  stack_size;
    char     default_tool[ICON_MAX_STR];
    char     tool_window[ICON_MAX_STR];
    char     tooltypes[ICON_MAX_TT][ICON_MAX_TT_LEN];
    int      tooltype_count;

    uint8_t  has_drawer;                  /* DrawerData present            */
    uint32_t drawer_off;                  /* file offset of DrawerData(56) */
    uint8_t  has_dd2;                     /* trailing flags/viewmodes      */
    uint32_t dd2_flags;
    uint16_t dd2_viewmodes;

    uint32_t gadget_render;               /* stale ptr — nonzero = present */
    uint32_t select_render;
    uint32_t img_off;                     /* file offset of normal Image   */
    uint32_t img_end;                     /* end of its plane data         */
    uint32_t sel_off;                     /* selected Image, 0 = none      */
    uint32_t sel_end;
    uint32_t userdata;                    /* gadget UserData (DD2 gate)    */
} IconMeta;

/* Parse a raw .info buffer.  Returns 1 on success. */
int  Icon_ParseBuf(const uint8_t *data, uint32_t size, IconMeta *out);

/* Read "<path>.info" into buf (VFS, any volume).  Returns size or 0. */
uint32_t Icon_ReadInfo(const char *path, uint8_t *buf, uint32_t max);

/* Load "<path>.info" metadata.  Returns 1 on success. */
int  Icon_LoadMeta(const char *path, IconMeta *out);

/* Register the icon.library ROM module (called by UAOS_ROM_RegisterAll). */
void UAOS_ICON_Register(void);

#endif /* UAOS_ICON_LIB_H */
