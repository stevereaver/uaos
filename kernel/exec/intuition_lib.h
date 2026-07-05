/*
 * intuition_lib.h — UAOS intuition.library Structures
 *
 * Minimal AmigaOS-compatible Window / NewWindow definitions plus the
 * ROM module registration hook.
 */

#ifndef UAOS_INTUITION_LIB_H
#define UAOS_INTUITION_LIB_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Register intuition.library
 * ------------------------------------------------------------------------- */
void UAOS_INTUITION_Register(void);

/* Render the front Intuition screen's custom BitMap into the host framebuffer.
 * Returns 1 if a screen bitmap was rendered, 0 otherwise. */
int UAOS_Intuition_RenderScreenBackdrop(void);

/* Apply the frontmost screen's SA_Colors/SA_Colors32/SA_Pens palette to the
 * host Workbench palette globals.  Falls back to the default palette if no
 * front screen has custom colors. */
void UAOS_Intuition_ApplyFrontScreenPalette(void);

/* Guest heap allocator shared by intuition.library and gadtools.library. */
uint32_t intu_alloc(uint32_t size);
void     intu_free(uint32_t user_addr);

/* Dispatch entry point for m68k Intuition calls (used by gadtools wrappers). */
void UAOS_Intuition_Dispatch(uint32_t fn);

/* -------------------------------------------------------------------------
 * Minimal AmigaOS NewWindow struct (classic layout)
 * ------------------------------------------------------------------------- */
typedef struct {
    int16_t  LeftEdge;        /*  0 */
    int16_t  TopEdge;         /*  2 */
    int16_t  Width;           /*  4 */
    int16_t  Height;          /*  6 */
    uint8_t  DetailPen;       /*  8 */
    uint8_t  BlockPen;        /*  9 */
    uint16_t IDCMPFlags;      /* 10 */
    uint16_t Flags;           /* 12 */
    uint32_t FirstGadget;     /* 14 */
    uint32_t CheckMark;       /* 18 */
    uint32_t Title;           /* 22 */
    uint32_t Screen;          /* 26 */
    uint32_t BitMap;          /* 30 */
    int16_t  MinWidth;        /* 34 */
    int16_t  MinHeight;       /* 36 */
    int16_t  MaxWidth;        /* 38 */
    int16_t  MaxHeight;       /* 40 */
    uint16_t Type;            /* 42 */
} AmigaNewWindow;

/* -------------------------------------------------------------------------
 * Minimal AmigaOS Window struct — offsets match AmigaOS 3.x up to the
 * IDCMP/UserPort fields so guest programs can dereference them.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t NextWindow;      /*  0 */
    int16_t  LeftEdge;        /*  4 */
    int16_t  TopEdge;         /*  6 */
    int16_t  Width;           /*  8 */
    int16_t  Height;          /* 10 */
    int16_t  MouseY;          /* 12 */
    int16_t  MouseX;          /* 14 */
    int16_t  MinWidth;        /* 16 */
    int16_t  MinHeight;       /* 18 */
    uint16_t MaxWidth;        /* 20 */
    uint16_t MaxHeight;       /* 22 */
    uint32_t Flags;           /* 24 */
    uint32_t MenuStrip;       /* 28 */
    uint32_t Title;           /* 32 */
    uint32_t FirstRequest;    /* 36 */
    uint32_t DMRequest;       /* 40 */
    int16_t  ReqCount;        /* 44 */
    uint32_t WScreen;         /* 46 */
    uint32_t RPort;           /* 50 */
    uint8_t  BorderLeft;      /* 54 */
    uint8_t  BorderTop;       /* 55 */
    uint8_t  BorderRight;     /* 56 */
    uint8_t  BorderBottom;    /* 57 */
    uint32_t BorderRPort;     /* 58 */
    uint32_t FirstGadget;     /* 62 */
    uint32_t Parent;          /* 66 */
    uint32_t Descendant;      /* 70 */
    uint32_t Pointer;         /* 74 */
    uint8_t  PtrHeight;       /* 78 */
    uint8_t  PtrWidth;        /* 79 */
    uint8_t  XOffset;         /* 80 */
    uint8_t  YOffset;         /* 81 */
    uint32_t IDCMPFlags;      /* 82 */
    uint32_t UserPort;        /* 86 */
    uint32_t WindowPort;      /* 90 */
    uint32_t MessageKey;      /* 94 */
    uint8_t  DetailPen;       /* 98 */
    uint8_t  BlockPen;        /* 99 */
    uint8_t  Pad1[38];        /* 100-137 */
} AmigaWindow;

/* Window field offsets (for code that prefers explicit offsets) */
#define WIN_OFF_NEXTWINDOW   0
#define WIN_OFF_LEFTEDGE     4
#define WIN_OFF_TOPEDGE      6
#define WIN_OFF_WIDTH        8
#define WIN_OFF_HEIGHT      10
#define WIN_OFF_IDCMPFLAGS  82
#define WIN_OFF_FLAGS       24
#define WIN_OFF_MENUSTRIP   28
#define WIN_OFF_TITLE       32
#define WIN_OFF_FIRSTREQUEST 36
#define WIN_OFF_REQCOUNT    44
#define WIN_OFF_WSCREEN     46
#define WIN_OFF_RPORT       50
#define WIN_OFF_FIRSTGADGET 62
#define WIN_OFF_USERPORT    86
#define WIN_OFF_WINDOWPORT  90
#define WIN_OFF_MESSAGEKEY  94
#define WIN_OFF_DETAILPEN   98
#define WIN_OFF_BLOCKPEN    99

/* Window flags (AmigaOS 3.x compatible) */
#define WFLG_CLOSEGADGET     0x0001
#define WFLG_DRAGBAR         0x0002
#define WFLG_DEPTHGADGET     0x0004
#define WFLG_SIZEGADGET      0x0008
#define WFLG_SIZEBRIGHT      0x0010
#define WFLG_SIZEBBOTTOM     0x0020
#define WFLG_REFRESHBITS     0x00C0
#define WFLG_SMART_REFRESH   0x0000
#define WFLG_SIMPLE_REFRESH  0x0040
#define WFLG_SUPER_BITMAP    0x0080
#define WFLG_OTHER_REFRESH   0x00C0
#define WFLG_BACKDROP        0x0100
#define WFLG_REPORTMOUSE     0x0200
#define WFLG_GIMMEZEROZERO   0x0400
#define WFLG_BORDERLESS      0x0800
#define WFLG_ACTIVATE        0x1000
#define WFLG_NOCAREREFRESH   0x2000
#define WFLG_NW_EXTENDED     0x4000
#define WFLG_NEWLOOKMENUS    0x8000
#define WFLG_RMBTRAP         0x00010000
#define WFLG_WBENCHWINDOW    0x02000000

/* IDCMP flags */
#define IDCMP_SIZEVERIFY     0x00000001
#define IDCMP_NEWSIZE        0x00000002
#define IDCMP_REFRESHWINDOW  0x00000004
#define IDCMP_MOUSEBUTTONS   0x00000008
#define IDCMP_MOUSEMOVE      0x00000010
#define IDCMP_GADGETDOWN     0x00000020
#define IDCMP_GADGETUP       0x00000040
#define IDCMP_MENUPICK       0x00000080
#define IDCMP_CLOSEWINDOW    0x00000100
#define IDCMP_RAWKEY         0x00000200
#define IDCMP_REQVERIFY      0x00000400
#define IDCMP_REQSET         0x00000800
#define IDCMP_IDCMPUPDATE    0x00001000
#define IDCMP_DELTAMOVE      0x00002000
#define IDCMP_INTUITICKS     0x00004000
#define IDCMP_ACTIVEWINDOW   0x00008000
#define IDCMP_INACTIVEWINDOW 0x00010000
#define IDCMP_DISKINSERTED   0x00020000
#define IDCMP_DISKREMOVED    0x00040000
#define IDCMP_WBENCHMESSAGE  0x00080000
#define IDCMP_HELP           0x00100000
#define IDCMP_VANILLAKEY     0x00200000
#define IDCMP_MENUHELP       0x20000000
#define IDCMP_TABLET         0x40000000

/* -------------------------------------------------------------------------
 * Exec MsgPort / Message / IntuiMessage offsets
 * ------------------------------------------------------------------------- */
#define MP_OFF_LN_SUCC     0
#define MP_OFF_LN_PRED     4
#define MP_OFF_LN_TYPE     8
#define MP_OFF_LN_PRI      9
#define MP_OFF_LN_NAME    10
#define MP_OFF_FLAGS      14
#define MP_OFF_SIGBIT     15
#define MP_OFF_SIGTASK    16
#define MP_OFF_MSGLIST    20
#define MP_SIZE           34

#define LH_OFF_HEAD        0
#define LH_OFF_TAIL        4
#define LH_OFF_TAILPRED    8
#define LH_OFF_TYPE       12
#define LH_SIZE           14

#define MSG_OFF_LN_SUCC    0
#define MSG_OFF_LN_PRED    4
#define MSG_OFF_LN_TYPE    8
#define MSG_OFF_LN_PRI     9
#define MSG_OFF_LN_NAME   10
#define MSG_OFF_LENGTH    14
#define MSG_OFF_REPLYPORT 16
#define MSG_OFF_DATA      20
#define MSG_SIZE          24

#define IM_OFF_EXECMSG     0
#define IM_OFF_CLASS      24
#define IM_OFF_CODE       28
#define IM_OFF_QUALIFIER  30
#define IM_OFF_IADDRESS   32
#define IM_OFF_MOUSEX     36
#define IM_OFF_MOUSEY     38
#define IM_OFF_SECONDS    40
#define IM_OFF_MICROS     44
#define IM_OFF_IDCMPWINDOW 48
#define IM_OFF_SPECIALLINK 52
#define IM_SIZE           56

#define NT_MSGPORT         4

/* -------------------------------------------------------------------------
 * AmigaOS IntuiText structure offsets
 * ------------------------------------------------------------------------- */
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

/* -------------------------------------------------------------------------
 * AmigaOS Gadget structure offsets
 * ------------------------------------------------------------------------- */
#define GAD_OFF_NEXTGADGET  0
#define GAD_OFF_LEFTEDGE    4
#define GAD_OFF_TOPEDGE     6
#define GAD_OFF_WIDTH       8
#define GAD_OFF_HEIGHT     10
#define GAD_OFF_FLAGS      12
#define GAD_OFF_ACTIVATION 14
#define GAD_OFF_GADGETTYPE 16
#define GAD_OFF_GADGETRENDER 18
#define GAD_OFF_SELECTRENDER 22
#define GAD_OFF_GADGETTEXT 26
#define GAD_OFF_MUTUALEXCLUDE 30
#define GAD_OFF_SPECIALINFO 34
#define GAD_OFF_GADGETID   38
#define GAD_OFF_USERDATA   40
#define GAD_SIZE           44

#define GFLG_SYSGADGET     0x2000
#define GFLG_SELECTED      0x0040
#define GFLG_GADGETUP      0x0001
#define GFLG_GADGETDOWN    0x0002
#define GFLG_DISABLED      0x0080
#define GFLG_RELBOTTOM     0x0008
#define GFLG_RIGHTBORDER   0x0010
#define GFLG_LEFTBORDER    0x0020
#define GFLG_TOPBORDER     0x0100
#define GFLG_BOTTOMBORDER  0x0200
#define GFLG_STRINGEXTEND  0x0400
#define GFLG_IMAGEDISABLE  0x0800
#define GFLG_LABELITEXT    0x1000
#define GFLG_LABELIMAGE    0x4000
#define GFLG_LABELSTRING   0x6000

#define GACT_IMMEDIATE     0x0001
#define GACT_RELVERIFY     0x0002
#define GACT_TOGGLESELECT  0x0004
#define GACT_INTUITICKS    0x0040
#define GACT_STRINGLEFT    0x0000
#define GACT_STRINGRIGHT   0x0008
#define GACT_STRINGCENTER  0x0010
#define GACT_STRINGLONGEST 0x0018
#define GACT_RELTOP        0x0020

/* AmigaOS PropInfo structure offsets */
#define PROP_OFF_FLAGS       0
#define PROP_OFF_HORIZPOT    2
#define PROP_OFF_VERTPOT     4
#define PROP_OFF_HORIZBODY   6
#define PROP_OFF_VERTBODY    8
#define PROP_OFF_WIDTH       10
#define PROP_OFF_HEIGHT      12
#define PROP_OFF_HORIZSIG    14
#define PROP_OFF_VERTSIG     16
#define PROP_SIZE            18

/* AmigaOS StringInfo structure offsets (minimal) */
#define SI_OFF_BUFFER        0
#define SI_OFF_UNDOBUFFER    4
#define SI_OFF_BUFFERPOS      8
#define SI_OFF_MAXCHARS      10
#define SI_OFF_DISPPOS       12
#define SI_OFF_NUMCHARS      16
#define SI_OFF_MIN           20
#define SI_OFF_MAX           24
#define SI_SIZE              28

/* UAOS simple ListView gadget extension (stored in SpecialInfo) */
#define LV_OFF_ITEMS         0
#define LV_OFF_COUNT         4
#define LV_OFF_SELECTED      8
#define LV_OFF_VISIBLE      12
#define LV_OFF_TOP          16
#define LV_OFF_MULTI_SELECT 20
#define LV_OFF_SELECTED_MASK 24
#define LV_SIZE             28

#define PROP_FLAGS_AUTOKNOB  0x0001
#define PROP_FLAGS_FREEVERT  0x0002
#define PROP_FLAGS_FREEHORIZ  0x0004
#define PROP_FLAGS_PROPBORDERLESS 0x0008
#define PROP_FLAGS_KNOBHIT   0x0100
#define PROP_FLAGS_DRAWRELX  0x0200
#define PROP_FLAGS_DRAWRELY  0x0400


#define GTYP_SYSGADGET     0x8000
#define GTYP_SIZER          0x0000
#define GTYP_WDRAGGING      0x0001
#define GTYP_WDEPTH         0x0002
#define GTYP_WCLOSE         0x0004
#define GTYP_WZOOM          0x0008
#define GTYP_BOOLGADGET     0x0001
#define GTYP_PROPGADGET     0x0002
#define GTYP_INTGADGET      0x0003
#define GTYP_STRGADGET      0x0004
#define GTYP_CUSTOMGADGET   0x0005
#define GTYP_LISTVIEW       0x0006   /* UAOS simple listview as custom gadget subtype */

#define SYSGAD_CLOSE        1
#define SYSGAD_DRAG         2
#define SYSGAD_DEPTH        3
#define SYSGAD_SIZE         4

/* -------------------------------------------------------------------------
 * AmigaOS EasyStruct structure offsets
 * ------------------------------------------------------------------------- */
#define ES_OFF_STRUCTSIZE     0
#define ES_OFF_FLAGS          4
#define ES_OFF_TITLE          8
#define ES_OFF_TEXTFORMAT    12
#define ES_OFF_GADGETFORMAT  16
#define ES_SIZE              20

/* -------------------------------------------------------------------------
 * AmigaOS Menu / MenuItem structure offsets
 * ------------------------------------------------------------------------- */
#define MENU_OFF_NEXTMENU   0
#define MENU_OFF_LEFTEDGE   4
#define MENU_OFF_TOPEDGE    6
#define MENU_OFF_WIDTH      8
#define MENU_OFF_HEIGHT     10
#define MENU_OFF_FLAGS      12
#define MENU_OFF_MENUNAME   14
#define MENU_OFF_FIRSTITEM  18
#define MENU_OFF_SIZE       26

#define MENUITEM_OFF_NEXTITEM    0
#define MENUITEM_OFF_LEFTEDGE    4
#define MENUITEM_OFF_TOPEDGE     6
#define MENUITEM_OFF_WIDTH       8
#define MENUITEM_OFF_HEIGHT     10
#define MENUITEM_OFF_FLAGS      12
#define MENUITEM_OFF_MUTUALEX   14
#define MENUITEM_OFF_ITEMFILL   18
#define MENUITEM_OFF_SELECTFILL 22
#define MENUITEM_OFF_COMMAND    26
#define MENUITEM_OFF_SUBITEM    27
#define MENUITEM_OFF_NEXTSELECT 31
#define MENUITEM_OFF_SIZE       35

/* Menu number extraction (classic 16-bit menu numbers) */
#define MENUNULL 0xFFFF
#define NOMENU   0x001F
#define NOITEM   0x003F
#define NOSUB    0x001F
#define MENUNUM(n)  ((n) & 0x001F)
#define ITEMNUM(n)  (((n) >> 5) & 0x003F)
#define SUBNUM(n)   (((n) >> 11) & 0x001F)

/* MenuItem flags */
#define ITEMTEXT     0x0001
#define ITEMENABLED  0x0002
#define COMMSEQ      0x0004
#define CHECKIT      0x0008
#define MENUTOGGLE   0x0010
#define ITEMEXTENDED 0x0020

/* Menu flags */
#define MENUENABLED  0x0001
/* -------------------------------------------------------------------------
 * AmigaOS Requester structure offsets (packed, partial)
 * ------------------------------------------------------------------------- */
#define REQ_OFF_OLDERREQUEST  0
#define REQ_OFF_REQTITLE      4
#define REQ_OFF_REQTEXT       8
#define REQ_OFF_LEFTEDGE     12
#define REQ_OFF_TOPEDGE      14
#define REQ_OFF_WIDTH        16
#define REQ_OFF_HEIGHT       18
#define REQ_OFF_FLAGS        20
#define REQ_OFF_REQGADGET    22
#define REQ_OFF_REQGADGETS   24
#define REQ_SIZE             40

/* -------------------------------------------------------------------------
 * AmigaOS NewScreen structure offsets
 * ------------------------------------------------------------------------- */
#define NS_OFF_LEFTEDGE     0
#define NS_OFF_TOPEDGE      2
#define NS_OFF_WIDTH        4
#define NS_OFF_HEIGHT       6
#define NS_OFF_DEPTH        8
#define NS_OFF_DETAILPEN   10
#define NS_OFF_BLOCKPEN    11
#define NS_OFF_VIEWMODES   12
#define NS_OFF_TYPE        14
#define NS_OFF_FONT        16
#define NS_OFF_DEFAULTTITLE 20
#define NS_OFF_GADGETS     24
#define NS_SIZE            28

/* -------------------------------------------------------------------------
 * AmigaOS Screen structure offsets (packed, partial)
 * ------------------------------------------------------------------------- */
#define SCR_OFF_NEXTSCREEN    0
#define SCR_OFF_FIRSTWINDOW   4
#define SCR_OFF_LEFTEDGE      8
#define SCR_OFF_TOPEDGE      10
#define SCR_OFF_WIDTH        12
#define SCR_OFF_HEIGHT       14
#define SCR_OFF_MOUSEY       16
#define SCR_OFF_MOUSEX       18
#define SCR_OFF_FLAGS        20
#define SCR_OFF_TITLE        24
#define SCR_OFF_DEFAULTTITLE  28
#define SCR_OFF_BARHEIGHT    32
#define SCR_OFF_VBOR        33
#define SCR_OFF_HBOR        34
#define SCR_OFF_MVBOR       35
#define SCR_OFF_MHBOR       36
#define SCR_OFF_WBORTOP     37
#define SCR_OFF_WBORLEFT    38
#define SCR_OFF_WBORRIGHT   39
#define SCR_OFF_WBORBOTTOM  40
#define SCR_OFF_FONT         42
#define SCR_OFF_VIEWPORT     46
#define SCR_OFF_DETAILPEN    70
#define SCR_OFF_BLOCKPEN     71
#define SCR_OFF_RASTPORT     80
#define SCR_OFF_DEPTH        84
#define SCR_OFF_BITMA        88
#define SCR_OFF_DISPLAYID    92
#define SCR_OFF_COLORS       96
#define SCR_SIZE            256

/* -------------------------------------------------------------------------
 * AmigaOS DrawInfo structure offsets (packed, partial)
 * ------------------------------------------------------------------------- */
#define DRINFO_OFF_VERSION   0
#define DRINFO_OFF_NUMPENS   2
#define DRINFO_OFF_PENS      4
#define DRINFO_PEN_COUNT    16
#define DRINFO_OFF_FONT     36
#define DRINFO_OFF_DEPTH    40
#define DRINFO_OFF_RESX     42
#define DRINFO_OFF_RESY     44
#define DRINFO_OFF_FLAGS    48
#define DRINFO_SIZE         64

/* -------------------------------------------------------------------------
 * Screen attribute tags (SA_*)
 * ------------------------------------------------------------------------- */
#define SA_Dummy         (TAG_USER + 32)
#define SA_Left          (SA_Dummy + 0x0001)
#define SA_Top           (SA_Dummy + 0x0002)
#define SA_Width         (SA_Dummy + 0x0003)
#define SA_Height        (SA_Dummy + 0x0004)
#define SA_Depth         (SA_Dummy + 0x0005)
#define SA_DetailPen     (SA_Dummy + 0x0006)
#define SA_BlockPen      (SA_Dummy + 0x0007)
#define SA_Title         (SA_Dummy + 0x0008)
#define SA_Colors        (SA_Dummy + 0x0009)
#define SA_ErrorCode     (SA_Dummy + 0x000A)
#define SA_Font          (SA_Dummy + 0x000B)
#define SA_SysFont       (SA_Dummy + 0x000C)
#define SA_Type          (SA_Dummy + 0x000D)
#define SA_BitMap        (SA_Dummy + 0x000E)
#define SA_PubName       (SA_Dummy + 0x000F)
#define SA_PubSig        (SA_Dummy + 0x0010)
#define SA_PubTask       (SA_Dummy + 0x0011)
#define SA_DisplayID     (SA_Dummy + 0x0012)
#define SA_DClip         (SA_Dummy + 0x0013)
#define SA_Overscan      (SA_Dummy + 0x0014)

/* Overscan types for SA_Overscan */
#define OSCAN_TEXT       1
#define OSCAN_STANDARD   2
#define OSCAN_MAX        3
#define OSCAN_VIDEO      4

#define SA_Obsolete1     (SA_Dummy + 0x0015)
#define SA_ShowTitle     (SA_Dummy + 0x0016)
#define SA_Behind        (SA_Dummy + 0x0017)
#define SA_Quiet         (SA_Dummy + 0x0018)
#define SA_AutoScroll    (SA_Dummy + 0x0019)
#define SA_Pens          (SA_Dummy + 0x001A)
#define SA_FullPalette   (SA_Dummy + 0x001B)
#define SA_ColorMapEntries (SA_Dummy + 0x001C)
#define SA_Parent        (SA_Dummy + 0x001D)
#define SA_Draggable     (SA_Dummy + 0x001E)
#define SA_Exclusive     (SA_Dummy + 0x001F)
#define SA_SharePens     (SA_Dummy + 0x0020)
#define SA_BackFill      (SA_Dummy + 0x0021)
#define SA_Interleaved   (SA_Dummy + 0x0022)
#define SA_Colors32      (SA_Dummy + 0x0023)
#define SA_LikeWorkbench (SA_Dummy + 0x0027)
#define SA_Reserved      (SA_Dummy + 0x0028)
#define SA_MinimizeISG   (SA_Dummy + 0x0029)

/* ColorSpec offsets for SA_Colors (classic AmigaOS format) */
#define CS_OFF_BUFFER    0
#define CS_OFF_RED       2
#define CS_OFF_GREEN     4
#define CS_OFF_BLUE      6
#define CS_SIZE          8

/* DrawInfo pen indices for SA_Pens */
#define DRI_FILLPEN           0
#define DRI_TEXTPEN           1
#define DRI_SHINEPEN          2
#define DRI_SHADOWPEN           3
#define DRI_FILLTEXTPEN        4
#define DRI_BACKGROUNDPEN       5
#define DRI_HIGHLIGHTTEXTPEN    6
#define DRI_BARDETAILPEN        7
#define DRI_BARBLOCKPEN         8
#define DRI_BARTRIMPEN          9
#define DRI_MENUOVPEN          10
#define DRI_AMIGAOVERPEN       11
#define DRI_PEN_MAX            12

/* Screen type / flag bits */
#define CUSTOMSCREEN     0x0000
#define WBENCHSCREEN     0x0001
#define PUBLICSCREEN     0x0002
#define CUSTOMBITMAP     0x0008
#define SHOWTITLE        0x0010
#define BEHIND           0x0020
#define QUIET            0x0040
#define AUTOSCROLL       0x0080

/* -------------------------------------------------------------------------
 * TagItem for OpenWindowTagList() tag parsing
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t ti_Tag;
    uint32_t ti_Data;
} AmigaTagItem;

#define TAG_DONE  0
#define TAG_USER  0x80000000

#define WA_Dummy       (TAG_USER + 99)  /* 0x80000063 */
#define WA_Left        (WA_Dummy + 0x01)
#define WA_Top         (WA_Dummy + 0x02)
#define WA_Width       (WA_Dummy + 0x03)
#define WA_Height      (WA_Dummy + 0x04)
#define WA_DetailPen   (WA_Dummy + 0x05)
#define WA_BlockPen    (WA_Dummy + 0x06)
#define WA_IDCMP       (WA_Dummy + 0x07)
#define WA_Flags       (WA_Dummy + 0x08)
#define WA_Gadgets     (WA_Dummy + 0x09)
#define WA_Checkmark   (WA_Dummy + 0x0A)
#define WA_Title       (WA_Dummy + 0x0B)
#define WA_ScreenTitle (WA_Dummy + 0x0C)
#define WA_CustomScreen (WA_Dummy + 0x0D)
#define WA_SuperBitMap (WA_Dummy + 0x0E)
#define WA_MinWidth    (WA_Dummy + 0x0F)
#define WA_MinHeight   (WA_Dummy + 0x10)
#define WA_MaxWidth    (WA_Dummy + 0x11)
#define WA_MaxHeight   (WA_Dummy + 0x12)
#define WA_InnerWidth  (WA_Dummy + 0x13)
#define WA_InnerHeight (WA_Dummy + 0x14)
#define WA_PubScreenName   (WA_Dummy + 0x15)
#define WA_PubScreen       (WA_Dummy + 0x16)
#define WA_PubScreenFallBack (WA_Dummy + 0x17)
#define WA_WindowName  (WA_Dummy + 0x18)
#define WA_Colors      (WA_Dummy + 0x19)
#define WA_Zoom        (WA_Dummy + 0x1A)
#define WA_MouseQueue  (WA_Dummy + 0x1B)
#define WA_BackFill    (WA_Dummy + 0x1C)
#define WA_RptQueue    (WA_Dummy + 0x1D)
#define WA_SizeGadget  (WA_Dummy + 0x1E)
#define WA_DragBar     (WA_Dummy + 0x1F)
#define WA_DepthGadget (WA_Dummy + 0x20)
#define WA_CloseGadget (WA_Dummy + 0x21)
#define WA_Backdrop    (WA_Dummy + 0x22)
#define WA_ReportMouse (WA_Dummy + 0x23)
#define WA_NoCareRefresh (WA_Dummy + 0x24)
#define WA_Borderless  (WA_Dummy + 0x25)
#define WA_Activate    (WA_Dummy + 0x26)
#define WA_RMBTrap     (WA_Dummy + 0x27)
#define WA_WBenchWindow (WA_Dummy + 0x28)
#define WA_SimpleRefresh (WA_Dummy + 0x29)
#define WA_SmartRefresh (WA_Dummy + 0x2A)
#define WA_SizeBRight  (WA_Dummy + 0x2B)
#define WA_SizeBBottom (WA_Dummy + 0x2C)
#define WA_AutoAdjust  (WA_Dummy + 0x2D)
#define WA_GimmeZeroZero (WA_Dummy + 0x2E)
#define WA_MenuHelp    (WA_Dummy + 0x2F)
#define WA_NewLookMenus (WA_Dummy + 0x30)
#define WA_AmigaKey    (WA_Dummy + 0x31)
#define WA_NotifyDepth (WA_Dummy + 0x32)
#define WA_Pointer     (WA_Dummy + 0x34)
#define WA_BusyPointer (WA_Dummy + 0x35)
#define WA_PointerDelay (WA_Dummy + 0x36)
#define WA_TabletMessages (WA_Dummy + 0x37)
#define WA_HelpGroup   (WA_Dummy + 0x38)
#define WA_HelpGroupWindow (WA_Dummy + 0x39)

/* HelpControl() flags */
#define HC_GADGETHELP 0x00000001

/* ScreenNotify tags (StartScreenNotifyTagList) */
#define SN_Dummy       (TAG_USER + 0x50000)
#define SN_Type        (SN_Dummy + 0x01)
#define SN_Flags       (SN_Dummy + 0x02)
#define SN_Priority    (SN_Dummy + 0x03)
#define SN_UserData    (SN_Dummy + 0x04)
#define SN_SignalTask  (SN_Dummy + 0x05)
#define SN_SignalBit   (SN_Dummy + 0x06)
#define SN_Name        (SN_Dummy + 0x07)

#define SNOTIFY_TYPE_OPEN       0x00000001
#define SNOTIFY_TYPE_CLOSE      0x00000002
#define SNOTIFY_TYPE_DEPTH      0x00000004
#define SNOTIFY_TYPE_LOCKED     0x00000008
#define SNOTIFY_TYPE_SCREENTITLE 0x00000010
#define SNOTIFY_TYPE_ALL        0x0000001F

#define SNOTIFY_WAIT_REPLY 0x00000001
#define SNOTIFY_BEFORE_OPEN  0x00000002
#define SNOTIFY_AFTER_OPEN   0x00000004
#define SNOTIFY_BEFORE_CLOSE 0x00000008
#define SNOTIFY_AFTER_CLOSE  0x00000010

/* -------------------------------------------------------------------------
 * BOOPSI gadget attributes (GA_*)
 * ------------------------------------------------------------------------- */
#define GA_Dummy             (TAG_USER + 0x30000)
#define GA_Left              (GA_Dummy + 1)
#define GA_RelRight          (GA_Dummy + 2)
#define GA_Top               (GA_Dummy + 3)
#define GA_RelBottom         (GA_Dummy + 4)
#define GA_Width             (GA_Dummy + 5)
#define GA_RelWidth          (GA_Dummy + 6)
#define GA_Height            (GA_Dummy + 7)
#define GA_RelHeight         (GA_Dummy + 8)
#define GA_Text              (GA_Dummy + 9)
#define GA_IntuiText         (GA_Dummy + 10)
#define GA_Label             (GA_Dummy + 11)
#define GA_LabelImage        (GA_Dummy + 12)
#define GA_Image             (GA_Dummy + 13)
#define GA_Border            (GA_Dummy + 14)
#define GA_Title             (GA_Dummy + 15)
#define GA_GadgetPrint       (GA_Dummy + 16)
#define GA_SpecialInfo       (GA_Dummy + 17)
#define GA_ID                (GA_Dummy + 18)
#define GA_UserData          (GA_Dummy + 19)
#define GA_Next              (GA_Dummy + 20)
#define GA_Previous          (GA_Dummy + 21)
#define GA_DrawInfo          (GA_Dummy + 22)
#define GA_DisplayHook       (GA_Dummy + 23)
#define GA_HintControl       (GA_Dummy + 24)
#define GA_HintInfo          (GA_Dummy + 25)
#define GA_GZZGadgetsOnly    (GA_Dummy + 26)
#define GA_RelSpecial        (GA_Dummy + 27)
#define GA_Disabled          (GA_Dummy + 28)
#define GA_EndSlot           (GA_Dummy + 29)
#define GA_TabCycle          (GA_Dummy + 30)
#define GA_Highlight         (GA_Dummy + 31)
#define GA_Screen            (GA_Dummy + 32)
#define GA_Font              (GA_Dummy + 33)
#define GA_Underscore        (GA_Dummy + 34)
#define GA_ActivateKey       (GA_Dummy + 35)
#define GA_Immediate         (GA_Dummy + 36)
#define GA_RelVerify         (GA_Dummy + 37)
#define GA_Unique            (GA_Dummy + 38)
#define GA_ToggleSelect      (GA_Dummy + 39)
#define GA_SysGType          (GA_Dummy + 40)
#define GA_SysGadget         (GA_Dummy + 41)
#define GA_FrontGadget       (GA_Dummy + 42)
#define GA_BottomGadget      (GA_Dummy + 43)
#define GA_RightBorder       (GA_Dummy + 44)
#define GA_LeftBorder        (GA_Dummy + 45)
#define GA_TopBorder         (GA_Dummy + 46)
#define GA_BottomBorder      (GA_Dummy + 47)
#define GA_SysGadgetExtend   (GA_Dummy + 48)
#define GA_UserInput         (GA_Dummy + 49)
#define GA_HelpNode          (GA_Dummy + 50)
#define GA_HelpLine          (GA_Dummy + 51)
#define GA_MousePoint        (GA_Dummy + 52)
#define GA_Invalid           (GA_Dummy + 53)
#define GA_Selected          (GA_Dummy + 54)
#define GA_SysGadgetType     (GA_Dummy + 55)
#define GA_ViewGadget        (GA_Dummy + 56)
#define GA_Iconified         (GA_Dummy + 57)
#define GA_IconifyAfterOpen  (GA_Dummy + 58)
#define GA_IconifyAfterClose (GA_Dummy + 59)
#define GA_InCenterX        (GA_Dummy + 60)
#define GA_InCenterY        (GA_Dummy + 61)

/* -------------------------------------------------------------------------
 * pointerclass attributes (used for WA_Pointer custom pointer objects)
 * ------------------------------------------------------------------------- */
#define POINTERA_Dummy       (TAG_USER + 0x39000)
#define POINTERA_BitMap      (POINTERA_Dummy + 0x01)
#define POINTERA_XOffset     (POINTERA_Dummy + 0x02)
#define POINTERA_YOffset     (POINTERA_Dummy + 0x03)
#define POINTERA_WordWidth   (POINTERA_Dummy + 0x04)
#define POINTERA_XResolution (POINTERA_Dummy + 0x05)
#define POINTERA_YResolution (POINTERA_Dummy + 0x06)
#define POINTERA_Flags       (POINTERA_Dummy + 0x07)

/* -------------------------------------------------------------------------
 * menuclass attributes (MA_*)
 * ------------------------------------------------------------------------- */
#define MA_Dummy       (TAG_USER + 0x41000)
#define MA_Type        (MA_Dummy + 0x01)
#define MA_Label       (MA_Dummy + 0x02)
#define MA_Key         (MA_Dummy + 0x03)
#define MA_Command     (MA_Dummy + 0x04)
#define MA_Sub         (MA_Dummy + 0x05)
#define MA_ToggleTitle (MA_Dummy + 0x06)
#define MA_Checked     (MA_Dummy + 0x07)
#define MA_Disabled    (MA_Dummy + 0x08)
#define MA_Separator   (MA_Dummy + 0x09)
#define MA_AddChild    (MA_Dummy + 0x0A)
#define MA_RemChild    (MA_Dummy + 0x0B)
#define MA_ID          (MA_Dummy + 0x0C)

/* -------------------------------------------------------------------------
 * imageclass attributes (IA_*)
 * ------------------------------------------------------------------------- */
#define IA_Dummy       (TAG_USER + 0x40000)
#define IA_Left        (IA_Dummy + 0x01)
#define IA_Top         (IA_Dummy + 0x02)
#define IA_Width       (IA_Dummy + 0x03)
#define IA_Height      (IA_Dummy + 0x04)
#define IA_FGPen       (IA_Dummy + 0x05)
#define IA_BGPen       (IA_Dummy + 0x06)
#define IA_Data        (IA_Dummy + 0x07)
#define IA_Mode        (IA_Dummy + 0x08)
#define IA_SupportsDisable (IA_Dummy + 0x09)
#define IA_Normalize   (IA_Dummy + 0x0A)
/* IM_BitMap is used by some apps to pass a BitMap to imageclass */
#define IM_BitMap      (TAG_USER + 0x40010)

/* -------------------------------------------------------------------------
 * Alert types
 * ------------------------------------------------------------------------- */
#define RECOVERY_ALERT 0x00000001
#define DEADEND_ALERT  0x80000000

/* -------------------------------------------------------------------------
 * Screen depth/position flags
 * ------------------------------------------------------------------------- */
#define SDEPTH_TOFRONT  0
#define SDEPTH_TOBACK   1
#define SDEPTH_INFAMILY 2

#define SPOS_RELATIVE    0
#define SPOS_ABSOLUTE    1
#define SPOS_MAKEVISIBLE 2
#define SPOS_FORCEDRAG   4

/* -------------------------------------------------------------------------
 * AmigaOS Preferences structure offsets
 * ------------------------------------------------------------------------- */
#define PREF_SIZE              234
#define PREF_OFF_FONTHEIGHT     0
#define PREF_OFF_PRINTERPORT   1
#define PREF_OFF_BAUDRATE      2
#define PREF_OFF_KEYRPTSPEED   4
#define PREF_OFF_KEYRPTDELAY   12
#define PREF_OFF_DOUBLECLICK   20
#define PREF_OFF_POINTERMATRIX 28
#define PREF_OFF_XOFFSET       100
#define PREF_OFF_YOFFSET       101
#define PREF_OFF_COLOR17       102
#define PREF_OFF_COLOR18       104
#define PREF_OFF_COLOR19       106
#define PREF_OFF_POINTERTICKS  108
#define PREF_OFF_COLOR0        110
#define PREF_OFF_COLOR1        112
#define PREF_OFF_COLOR2        114
#define PREF_OFF_COLOR3        116
#define PREF_OFF_VIEWXOFFSET   118
#define PREF_OFF_VIEWYOFFSET   119
#define PREF_OFF_VIEWINITX     120
#define PREF_OFF_VIEWINITY     122
#define PREF_OFF_ENABLECLI     124
#define PREF_OFF_PRINTERTYPE   128
#define PREF_OFF_PRINTERFILENAME 130
#define PREF_OFF_PRINTPITCH    160
#define PREF_OFF_PRINTQUALITY  162
#define PREF_OFF_PRINTSPACING  164
#define PREF_OFF_PRINTLEFTMARGIN 166
#define PREF_OFF_PRINTRIGHTMARGIN 168
#define PREF_OFF_PRINTIMAGE  170
#define PREF_OFF_PRINTASPECT 172
#define PREF_OFF_PRINTSHADE  174
#define PREF_OFF_PRINTTHRESHOLD 176
#define PREF_OFF_PAPERSIZE   178
#define PREF_OFF_PAPERLENGTH 180
#define PREF_OFF_PAPERTYPE   182
#define PREF_OFF_SERRWBITS   184
#define PREF_OFF_SERSTOPBUF  185
#define PREF_OFF_SERPARSHK   186
#define PREF_OFF_LACEWB      187
#define PREF_OFF_PAD         188
#define PREF_OFF_PRTDEVNAME  200
#define PREF_OFF_DEFPRTUNIT  216
#define PREF_OFF_DEFSERUNIT  217
#define PREF_OFF_ROWSIZECHANGE 218
#define PREF_OFF_COLUMNSIZECHANGE 219
#define PREF_OFF_PRINTFLAGS  220
#define PREF_OFF_PRINTMAXWIDTH 222
#define PREF_OFF_PRINTMAXHEIGHT 224
#define PREF_OFF_PRINTDENSITY 226
#define PREF_OFF_PRINTXOFFSET 227
#define PREF_OFF_WBWIDTH     228
#define PREF_OFF_WBHEIGHT    230
#define PREF_OFF_WBDEPTH     232
#define PREF_OFF_EXTSIZE     233

/* -------------------------------------------------------------------------
 * BOOPSI structures and constants (minimal AmigaOS-compatible subset)
 * ------------------------------------------------------------------------- */

/* Object header (struct _Object) — precedes the instance data in memory. */
#define OBJ_OFF_LN_SUCC   0
#define OBJ_OFF_LN_PRED   4
#define OBJ_OFF_CLASS     8
#define OBJ_HEADER_SIZE   12

/* IClass field offsets (struct IClass) */
#define CLASS_OFF_DISPATCHER   0   /* struct Hook h_Entry at offset 8 within */
#define CLASS_OFF_DISPATCHER_ENTRY (CLASS_OFF_DISPATCHER + 8)
#define CLASS_OFF_SUBENTRY    12
#define CLASS_OFF_DATA        16
#define CLASS_OFF_RESERVED    20
#define CLASS_OFF_SUPER       24
#define CLASS_OFF_ID          28
#define CLASS_OFF_INST_OFFSET 32
#define CLASS_OFF_INST_SIZE   34
#define CLASS_OFF_USERDATA    36
#define CLASS_OFF_SUBCLASS_COUNT 40
#define CLASS_OFF_OBJECT_COUNT   44
#define CLASS_OFF_FLAGS       48
#define CLASS_OFF_NATIVE_DISPATCHER 52
#define CLASS_SIZE            56

/* CLASS_OFF_FLAGS bits */
#define CLASS_FLAG_NATIVE     0x0001

/* BOOPSI root methods */
#define OM_NEW         1
#define OM_DISPOSE     2
#define OM_SET         3
#define OM_GET         4
#define OM_ADDTAIL     5
#define OM_REMOVE      6
#define OM_ADDMEMBER   7
#define OM_REMMEMBER   8
#define OM_NOTIFY      9
#define OM_UPDATE      10

/* Gadget methods (GM_*) */
#define GM_HITTEST      0x64
#define GM_RENDER       0x65
#define GM_GOACTIVE     0x66
#define GM_HANDLEINPUT  0x67
#define GM_GOINACTIVE   0x68

/* BOOPSI messages (field offsets within the guest message block) */
#define MSG_OFF_METHODID   0

#define OPNEW_OFF_METHODID 0
#define OPNEW_OFF_ATTRLIST 4

#define OPSET_OFF_METHODID  0
#define OPSET_OFF_ATTRLIST  4
#define OPSET_OFF_GINFO     8

#define OPGET_OFF_METHODID  0
#define OPGET_OFF_ATTRID    4
#define OPGET_OFF_STORAGE   8

/* Dispatch entry point called from uaos_m68k_glue.c */
void UAOS_Intuition_Dispatch(uint32_t fn);

#endif /* UAOS_INTUITION_LIB_H */
