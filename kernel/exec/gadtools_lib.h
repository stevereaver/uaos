/*
 * gadtools_lib.h — UAOS gadtools.library structures and constants
 *
 * Minimal AmigaOS-compatible GadTools definitions used by the host
 * implementation in gadtools_lib.c.  Real AmigaOS programs use these
 * kinds, tags, and NewGadget offsets when calling gadtools.library.
 */

#ifndef UAOS_GADTOOLS_LIB_H
#define UAOS_GADTOOLS_LIB_H

#include <stdint.h>
#include "exec/intuition_lib.h"

/* -------------------------------------------------------------------------
 * Register gadtools.library
 * ------------------------------------------------------------------------- */
void UAOS_GADTOOLS_Register(void);

/* -------------------------------------------------------------------------
 * GadTools gadget kinds passed to CreateGadgetA()
 * ------------------------------------------------------------------------- */
#define GENERIC_KIND    0
#define BUTTON_KIND     1
#define CHECKBOX_KIND   2
#define INTEGER_KIND    3
#define LISTVIEW_KIND   4
#define MX_KIND         5
#define NUMBER_KIND     6
#define CYCLE_KIND      7
#define PALETTE_KIND    8
#define SCROLLER_KIND   9
/* kind number 10 is reserved */
#define SLIDER_KIND     11
#define RADIO_KIND      MX_KIND  /* alias */
#define STRING_KIND     12
#define TEXT_KIND       13
#define NUM_KINDS       14

/* -------------------------------------------------------------------------
 * NewGadget structure offsets
 * ------------------------------------------------------------------------- */
#define NG_OFF_LEFTEDGE     0
#define NG_OFF_TOPEDGE      2
#define NG_OFF_WIDTH        4
#define NG_OFF_HEIGHT       6
#define NG_OFF_GADGETTEXT   8
#define NG_OFF_TEXTATTR     12
#define NG_OFF_GADGETID     16
#define NG_OFF_FLAGS        18
#define NG_OFF_VISUALINFO   22
#define NG_OFF_USERDATA     26
#define NG_SIZE             30

/* IntuiText structure offsets */
#define ITEXT_OFF_FRONTPEN   0
#define ITEXT_OFF_BACKPEN    1
#define ITEXT_OFF_DRAWMODE   2
#define ITEXT_OFF_PAD        3
#define ITEXT_OFF_LEFTEDGE   4
#define ITEXT_OFF_TOPEDGE    6
#define ITEXT_OFF_FONT       8
#define ITEXT_OFF_ITEXT      12
#define ITEXT_OFF_NEXTTEXT   16
#define ITEXT_SIZE           20

/* NewGadget.ng_Flags bits */
#define NG_LOWLABEL     0x0001  /* label drawn below the gadget */
#define NG_HIGHLABEL    0x0002  /* label drawn above the gadget */
#define NG_LEFTLABEL    0x0004  /* label drawn to the left */
#define NG_RIGHTLABEL   0x0008  /* label drawn to the right */
#define NG_TEXT         0x0040  /* text/number gadget text */
#define NG_BORDER       0x0080  /* border around text/number gadget */
#define NG_TOGGLE       0x0100  /* button toggles on/off */
#define NG_DISABLED     0x0200  /* initially disabled */
#define NG_LIVE         0x0400  /* send GADGETDOWN while tracking */
#define NG_FILLLABEL    0x0800  /* fill label background */
#define NG_NOPFILL      0x1000  /* don't fill gadget background */
#define NG_STRINGCENTER 0x0008  /* center string text (alias) */
#define NG_STRINGRIGHT  0x0010  /* right justify string text (alias) */
#define NG_STRINGLEFT   0x0000  /* left justify string text (alias) */

/* -------------------------------------------------------------------------
 * GadTools tag base (TAG_USER + 0x80000)
 * ------------------------------------------------------------------------- */
#define GT_TagBase       (TAG_USER + 0x80000)

#define GT_Private0      (GT_TagBase + 3)
#define GTLV_Top         (GT_TagBase + 5)
#define GTLV_Labels      (GT_TagBase + 6)
#define GTLV_ReadOnly    (GT_TagBase + 7)
#define GTLV_ScrollWidth (GT_TagBase + 8)
#define GTMX_Labels      (GT_TagBase + 9)
#define GTMX_Active      (GT_TagBase + 10)
#define GTTX_Text        (GT_TagBase + 11)
#define GTTX_CopyText    (GT_TagBase + 12)
#define GTNM_Number      (GT_TagBase + 13)
#define GTCY_Labels      (GT_TagBase + 14)
#define GTCY_Active      (GT_TagBase + 15)
#define GTPA_Depth       (GT_TagBase + 16)
#define GTPA_Color       (GT_TagBase + 17)
#define GTPA_ColorOffset (GT_TagBase + 18)
#define GTPA_IndicatorWidth  (GT_TagBase + 19)
#define GTPA_IndicatorHeight (GT_TagBase + 20)
#define GTSC_Top         (GT_TagBase + 21)
#define GTSC_Total       (GT_TagBase + 22)
#define GTSC_Visible     (GT_TagBase + 23)
#define GTSC_Overlap     (GT_TagBase + 24)
#define GTSL_Min         (GT_TagBase + 38)
#define GTSL_Max         (GT_TagBase + 39)
#define GTSL_Level       (GT_TagBase + 40)
#define GTSL_MaxLevelLen (GT_TagBase + 41)
#define GTSL_LevelFormat (GT_TagBase + 42)
#define GTSL_LevelPlace  (GT_TagBase + 43)
#define GTSL_DispFunc    (GT_TagBase + 44)
#define GTST_String      (GT_TagBase + 45)
#define GTST_MaxChars    (GT_TagBase + 46)
#define GTIN_Number      (GT_TagBase + 47)
#define GTIN_MaxChars    (GT_TagBase + 48)
#define GTST_EditHook    (GT_TagBase + 55)
#define GTIN_EditHook    (GTST_EditHook)
#define GT_VisualInfo    (GT_TagBase + 52)
#define GTLV_ShowSelected (GT_TagBase + 53)
#define GTLV_Selected    (GT_TagBase + 54)
#define GT_Reserved1     (GT_TagBase + 56)
#define GTTX_Border      (GT_TagBase + 57)
#define GTNM_Border      (GT_TagBase + 58)
#define GTSC_Arrows      (GT_TagBase + 59)
#define GTMX_Spacing     (GT_TagBase + 61)
#define GT_Underscore    (GT_TagBase + 64)
#define GTMX_Scaled      (GT_TagBase + 69)
#define GTPA_NumColors   (GT_TagBase + 70)
#define GTMX_TitlePlace  (GT_TagBase + 71)
#define GTTX_FrontPen    (GT_TagBase + 72)
#define GTTX_BackPen     (GT_TagBase + 73)
#define GTTX_Justification (GT_TagBase + 74)
#define GTNM_FrontPen    (GT_TagBase + 72)
#define GTNM_BackPen     (GT_TagBase + 73)
#define GTNM_Justification (GT_TagBase + 74)
#define GTNM_Format      (GT_TagBase + 75)
#define GTNM_MaxNumberLen (GT_TagBase + 76)
#define GTLV_MakeVisible (GT_TagBase + 78)
#define GTLV_ItemHeight  (GT_TagBase + 79)
#define GTSL_MaxPixelLen (GT_TagBase + 80)
#define GTSL_Justification (GT_TagBase + 81)
#define GTPA_ColorTable  (GT_TagBase + 82)
#define GTLV_CallBack    (GT_TagBase + 83)
#define GTLV_MaxPen      (GT_TagBase + 84)
#define GTTX_Clipped     (GT_TagBase + 85)
#define GTNM_Clipped     (GT_TagBase + 85)
#define GT_Reserved0     (GTST_EditHook)

/* GTSL_LevelPlace values */
#define GTSL_LEVEL_NONE   0
#define GTSL_LEVEL_LEFT   1
#define GTSL_LEVEL_RIGHT  2
#define GTSL_LEVEL_BOTTOM 3
#define GTSL_LEVEL_TOP    4
#define GTSL_LEVEL_ENDS   5

/* GTTX/GTNM justification */
#define GTJ_LEFT    0
#define GTJ_RIGHT   1
#define GTJ_CENTER  2

/* -------------------------------------------------------------------------
 * GadTools menu tags
 * ------------------------------------------------------------------------- */
#define GTMN_FrontPen       (GT_TagBase + 25)
#define GTMN_BackPen        (GT_TagBase + 26)
#define GTMN_MenuID         (GT_TagBase + 27)
#define GTMN_Flags          (GT_TagBase + 28)
#define GTMN_NewLookMenus   (GT_TagBase + 60)
#define GTMN_TextAttr       (GT_TagBase + 62)

/* -------------------------------------------------------------------------
 * GadTools visual info (must match VisualInfo in intuition_lib.c)
 * ------------------------------------------------------------------------- */
#define GTVI_SIZE        16
#define GTVI_OFF_SCREEN   0
#define GTVI_OFF_DRAWINFO 4
#define GTVI_OFF_FONT     8
#define GTVI_OFF_FLAGS   12

/* -------------------------------------------------------------------------
 * GadTools function indices (1-based, must match LVO wiring in glue)
 * ------------------------------------------------------------------------- */
#define GADTOOLS_OPEN_LIBRARY          1
#define GADTOOLS_CLOSE_LIBRARY         2
#define GADTOOLS_CREATE_GADGET_A       3
#define GADTOOLS_FREE_GADGETS          4
#define GADTOOLS_GT_SET_GADGET_ATTRS_A 5
#define GADTOOLS_CREATE_MENUS_A        6
#define GADTOOLS_FREE_MENUS            7
#define GADTOOLS_LAYOUT_MENU_ITEMS_A   8
#define GADTOOLS_LAYOUT_MENUS_A        9
#define GADTOOLS_GT_GET_IMSG          10
#define GADTOOLS_GT_REPLY_IMSG         11
#define GADTOOLS_GT_REFRESH_WINDOW     12
#define GADTOOLS_GT_BEGIN_REFRESH      13
#define GADTOOLS_GT_END_REFRESH        14
#define GADTOOLS_GT_FILTER_IMSG       15
#define GADTOOLS_GT_POST_FILTER_IMSG   16
#define GADTOOLS_CREATE_CONTEXT        17
#define GADTOOLS_DRAW_BEVEL_BOX_A      18
#define GADTOOLS_GET_VISUAL_INFO_A     19
#define GADTOOLS_FREE_VISUAL_INFO       20
#define GADTOOLS_GT_GET_GADGET_ATTRS_A 21

#define GADTOOLS_MAX_FUNC 21

#endif /* UAOS_GADTOOLS_LIB_H */
