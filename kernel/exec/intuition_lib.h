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
 * Minimal AmigaOS Window struct — RPort placed at offset 50 to match
 * real AmigaOS 3.x layout so guest programs can dereference it.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t NextWindow;      /*  0 */
    int16_t  LeftEdge;        /*  4 */
    int16_t  TopEdge;         /*  6 */
    int16_t  Width;           /*  8 */
    int16_t  Height;          /* 10 */
    uint8_t  DetailPen;       /* 12 */
    uint8_t  BlockPen;        /* 13 */
    uint16_t IDCMPFlags;      /* 14 */
    uint16_t Flags;           /* 16 */
    uint32_t FirstGadget;     /* 18 */
    uint32_t CheckMark;       /* 22 */
    uint32_t Title;           /* 26 */
    uint32_t FirstRequest;    /* 30 */
    int16_t  ReqCount;        /* 34 */
    int16_t  Pad0;            /* 36 */
    uint32_t WScreen;         /* 38 */
    uint8_t  Pad1[8];         /* 42-49 */
    uint32_t RPort;           /* 50 */
    uint8_t  Pad2[14];        /* 54-67 */
} AmigaWindow;

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
#define IDCMP_SIZEVERIFY     0x0001
#define IDCMP_NEWSIZE        0x0002
#define IDCMP_REFRESHWINDOW  0x0004
#define IDCMP_MOUSEBUTTONS   0x0008
#define IDCMP_MOUSEMOVE      0x0010
#define IDCMP_GADGETDOWN     0x0020
#define IDCMP_GADGETUP       0x0040
#define IDCMP_MENUPICK       0x0080
#define IDCMP_CLOSEWINDOW    0x0100
#define IDCMP_RAWKEY         0x0200
#define IDCMP_REQVERIFY      0x0400
#define IDCMP_REQSET         0x0800
#define IDCMP_IDCMPUPDATE    0x1000
#define IDCMP_DELTAMOVE      0x2000
#define IDCMP_INTUITICKS     0x4000
#define IDCMP_ACTIVEWINDOW   0x8000

/* Dispatch entry point called from uaos_m68k_glue.c */
void UAOS_Intuition_Dispatch(uint32_t fn);

#endif /* UAOS_INTUITION_LIB_H */
