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
#define WIN_OFF_WSCREEN     48
#define WIN_OFF_RPORT       50
#define WIN_OFF_FIRSTGADGET 62
#define WIN_OFF_USERPORT    86
#define WIN_OFF_WINDOWPORT  90
#define WIN_OFF_MESSAGEKEY  94
#define WIN_OFF_DETAILPEN   98
#define WIN_OFF_BLOCKPEN    99

/* Window flags */
#define WFLG_CLOSEGADGET     0x0001
#define WFLG_DRAGBAR         0x0002
#define WFLG_DEPTHGADGET     0x0004
#define WFLG_SIZEGADGET      0x0008
#define WFLG_SIZEBRIGHT      0x0010
#define WFLG_SIZEBBOTTOM     0x0020
#define WFLG_SIMPLE_REFRESH  0x0040
#define WFLG_SMART_REFRESH   0x0080
#define WFLG_ACTIVATE        0x0100
#define WFLG_GIMMEZEROZERO   0x0200
#define WFLG_NOCAREREFRESH   0x0400
#define WFLG_NW_EXTENDED     0x0800
#define WFLG_NEWLOOKMENUS    0x1000
#define WFLG_BORDERLESS      0x2000
#define WFLG_BACKDROP        0x4000
#define WFLG_REPORTMOUSE     0x8000

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
#define IDCMP_VANILLAKEY     0x00200000

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

#define GACT_IMMEDIATE     0x0001
#define GACT_RELVERIFY     0x0002

#define GTYP_SYSGADGET     0x8000
#define GTYP_SIZER          0x0000
#define GTYP_WDRAGGING      0x0001
#define GTYP_WDEPTH         0x0002
#define GTYP_WCLOSE         0x0004
#define GTYP_WZOOM          0x0008
#define GTYP_BOOLGADGET     0x0001

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
#define SCR_SIZE            256

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
#define SA_BitMap        (SA_Dummy + 0x0009)
#define SA_Behind        (SA_Dummy + 0x000A)
#define SA_Quiet         (SA_Dummy + 0x000B)
#define SA_ShowTitle     (SA_Dummy + 0x000C)
#define SA_Type          (SA_Dummy + 0x000D)
#define SA_AutoScroll    (SA_Dummy + 0x000E)
#define SA_PubName       (SA_Dummy + 0x000F)
#define SA_Font          (SA_Dummy + 0x001B)

/* Screen type / flag bits */
#define CUSTOMSCREEN     0x0000
#define WBENCHSCREEN     0x0001
#define PUBLICSCREEN     0x0002
#define SHOWTITLE        0x0010
#define BEHIND           0x0020
#define QUIET            0x0040

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
#define WA_MinWidth    (WA_Dummy + 0x0F)
#define WA_MinHeight   (WA_Dummy + 0x10)
#define WA_MaxWidth    (WA_Dummy + 0x11)
#define WA_MaxHeight   (WA_Dummy + 0x12)
#define WA_PubScreen       (WA_Dummy + 0x16)
#define WA_PubScreenName   (WA_Dummy + 0x17)
#define WA_PubScreenFallBack (WA_Dummy + 0x18)
#define WA_Pointer         (WA_Dummy + 0x34)
#define WA_BusyPointer     (WA_Dummy + 0x35)
#define WA_PointerDelay    (WA_Dummy + 0x36)

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

/* Dispatch entry point called from uaos_m68k_glue.c */
void UAOS_Intuition_Dispatch(uint32_t fn);

#endif /* UAOS_INTUITION_LIB_H */
