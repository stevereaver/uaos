; CopperBars.s - Amiga Copper Bars demo for UAOS
;
; Opens an Intuition window and renders animated horizontal colour bars
; using graphics.library RectFill.  The bars bounce up and down within
; the window's content area.  Close the window's close gadget to exit.
;
; This version draws directly into the window's RastPort instead of
; taking over the Copper, so it coexists peacefully with the desktop.
;
; Assemble: vasmm68k_mot -Fhunk -o CopperBars.o CopperBars.s
; Link:     vlink -bamigahunk -o CopperBars CopperBars.o
;

; ---------------------------------------------------------------------------
; Library bases returned by OpenLibrary
; ---------------------------------------------------------------------------
EXEC_BASE       equ $00000300
DOS_BASE        equ $00000800
GRAPHICS_BASE   equ $00008000
INTUITION_BASE  equ $00009000

; ---------------------------------------------------------------------------
; Exec LVOs (standard AmigaOS offsets)
; ---------------------------------------------------------------------------
LVO_OpenLibrary  equ -552
LVO_CloseLibrary equ -414
LVO_GetMsg       equ -372
LVO_ReplyMsg     equ -378

; ---------------------------------------------------------------------------
; Graphics LVOs (UAOS project slot mapping: LVO = -slot * 6)
; ---------------------------------------------------------------------------
LVO_SetAPen      equ -342   ; slot 57
LVO_SetDrMd      equ -354   ; slot 59
LVO_RectFill     equ -306   ; slot 51
LVO_WaitTOF      equ -270   ; slot 45

; ---------------------------------------------------------------------------
; Intuition LVOs (standard AmigaOS offsets)
; ---------------------------------------------------------------------------
LVO_OpenWindow   equ -204
LVO_CloseWindow  equ -72
LVO_ModifyIDCMP  equ -174

; ---------------------------------------------------------------------------
; DOS LVO
; ---------------------------------------------------------------------------
LVO_DOS_Exit     equ -144

; ---------------------------------------------------------------------------
; IDCMP and window flags
; ---------------------------------------------------------------------------
IDCMP_CLOSEWINDOW equ $00000200

WFLG_CLOSEGADGET    equ $00000040
WFLG_DRAGBAR        equ $00000002
WFLG_DEPTHGADGET    equ $00000004
WFLG_ACTIVATE       equ $00001000
WFLG_NOCAREREFRESH  equ $00002000
WFLG_GIMMEZEROZERO  equ $00000400

; ---------------------------------------------------------------------------
; Window structure offsets
; ---------------------------------------------------------------------------
WIN_OFF_RPORT         equ 50
WIN_OFF_USERPORT      equ 86
WIN_OFF_WIDTH         equ 8
WIN_OFF_HEIGHT        equ 10
WIN_OFF_BORDERLEFT    equ 54
WIN_OFF_BORDERTOP     equ 55
WIN_OFF_BORDERRIGHT   equ 56
WIN_OFF_BORDERBOTTOM  equ 57

; ---------------------------------------------------------------------------
; IntuiMessage offsets
; ---------------------------------------------------------------------------
IM_OFF_CLASS    equ 24

; ---------------------------------------------------------------------------
; NewWindow structure offsets
; ---------------------------------------------------------------------------
NW_LEFTEDGE     equ 0
NW_TOPEDGE      equ 2
NW_WIDTH        equ 4
NW_HEIGHT       equ 6
NW_DETAILPEN    equ 8
NW_BLOCKPEN     equ 9
NW_IDCMPFLAGS   equ 10
NW_FLAGS        equ 12
NW_FIRSTGADGET  equ 14
NW_CHECKMARK    equ 18
NW_TITLE        equ 22
NW_SCREEN       equ 26
NW_BITMAP       equ 30
NW_MINWIDTH     equ 34
NW_MINHEIGHT    equ 36
NW_MAXWIDTH     equ 38
NW_MAXHEIGHT    equ 40
NW_TYPE         equ 42

; ---------------------------------------------------------------------------
; Drawing constants
; ---------------------------------------------------------------------------
JAM2            equ 1
BAR_COUNT       equ 6
WIN_W           equ 340
WIN_H           equ 240

; Pen colours from the fixed 16-colour Amiga palette
PEN_BLACK       equ 0
PEN_RED         equ 5
PEN_GREEN       equ 4
PEN_BLUE        equ 2
PEN_YELLOW      equ 3
PEN_MAGENTA     equ 10
PEN_CYAN        equ 9

        section bss,bss

exec_base:      ds.l 1
intuition_base: ds.l 1
graphics_base:  ds.l 1
window:         ds.l 1
rport:          ds.l 1
win_width:      ds.w 1          ; inner content width  (inside borders)
win_height:     ds.w 1          ; inner content height (inside borders)
newwindow:      ds.b 44

        section code,code

start:
        ; --- Open exec.library ------------------------------------------
        move.l  #libname_exec,a1
        moveq   #0,d0
        movea.l #EXEC_BASE,a6
        jsr     LVO_OpenLibrary(a6)
        movea.l d0,a6
        beq.w   exit
        move.l  a6,exec_base

        ; --- Open intuition.library -------------------------------------
        move.l  #libname_intuition,a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,intuition_base
        beq.w   close_exec

        ; --- Open graphics.library --------------------------------------
        move.l  #libname_graphics,a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,graphics_base
        beq.w   close_intuition

        ; --- Fill in the NewWindow structure ----------------------------
        lea     newwindow,a0
        move.w  #80,NW_LEFTEDGE(a0)
        move.w  #40,NW_TOPEDGE(a0)
        move.w  #WIN_W,NW_WIDTH(a0)
        move.w  #WIN_H,NW_HEIGHT(a0)
        move.b  #1,NW_DETAILPEN(a0)
        move.b  #0,NW_BLOCKPEN(a0)
        move.w  #IDCMP_CLOSEWINDOW,NW_IDCMPFLAGS(a0)
        move.l  #(WFLG_CLOSEGADGET|WFLG_DRAGBAR|WFLG_DEPTHGADGET|WFLG_ACTIVATE|WFLG_NOCAREREFRESH|WFLG_GIMMEZEROZERO),NW_FLAGS(a0)
        clr.l   NW_FIRSTGADGET(a0)
        clr.l   NW_CHECKMARK(a0)
        move.l  #win_title,a1
        move.l  a1,NW_TITLE(a0)
        clr.l   NW_SCREEN(a0)
        clr.l   NW_BITMAP(a0)
        move.w  #80,NW_MINWIDTH(a0)
        move.w  #60,NW_MINHEIGHT(a0)
        move.w  #640,NW_MAXWIDTH(a0)
        move.w  #400,NW_MAXHEIGHT(a0)
        clr.w   NW_TYPE(a0)

        ; --- Open the window --------------------------------------------
        movea.l intuition_base,a6
        movea.l newwindow,a0
        jsr     LVO_OpenWindow(a6)
        move.l  d0,window
        beq.w   close_graphics

        ; --- Ensure close events are delivered --------------------------
        movea.l window,a0
        move.l  #IDCMP_CLOSEWINDOW,d0
        jsr     LVO_ModifyIDCMP(a6)

        ; --- Get the window's RastPort ----------------------------------
        movea.l window,a0
        move.l  WIN_OFF_RPORT(a0),d0
        move.l  d0,rport

        ; --- Compute inner content dimensions ---------------------------
        ; With GIMMEZEROZERO, drawing (0,0) is inside the borders.
        ; inner_w = Width - BorderLeft - BorderRight
        ; inner_h = Height - BorderTop  - BorderBottom
        movea.l window,a0
        moveq   #0,d0
        moveq   #0,d1
        move.b  WIN_OFF_BORDERLEFT(a0),d0
        move.b  WIN_OFF_BORDERRIGHT(a0),d1
        move.w  WIN_OFF_WIDTH(a0),d2
        sub.w   d0,d2
        sub.w   d1,d2
        move.w  d2,win_width

        moveq   #0,d0
        moveq   #0,d1
        move.b  WIN_OFF_BORDERTOP(a0),d0
        move.b  WIN_OFF_BORDERBOTTOM(a0),d1
        move.w  WIN_OFF_HEIGHT(a0),d2
        sub.w   d0,d2
        sub.w   d1,d2
        move.w  d2,win_height

        ; --- Set draw mode to JAM2 (opaque) -----------------------------
        movea.l graphics_base,a6
        movea.l rport,a1
        moveq   #JAM2,d0
        jsr     LVO_SetDrMd(a6)

        ; ------------------------------------------------------------------
        ; Main animation loop
        ; ------------------------------------------------------------------
main_loop:
        ; --- Check for a close-window IDCMP message ---------------------
        movea.l exec_base,a6
        movea.l window,a0
        movea.l WIN_OFF_USERPORT(a0),a0
        jsr     LVO_GetMsg(a6)
        tst.l   d0
        beq.s   no_msg
        ; Got a message — read class, reply, then check
        move.l  d0,a0                  ; a0 = message
        move.l  IM_OFF_CLASS(a0),d1    ; d1 = message class
        move.l  a0,a1                  ; a1 = message for ReplyMsg
        movea.l exec_base,a6
        jsr     LVO_ReplyMsg(a6)
        and.l   #IDCMP_CLOSEWINDOW,d1
        bne.w   do_close

no_msg:
        ; --- Render the bars --------------------------------------------
        bsr     render_bars

        ; --- Pace the animation at ~50 Hz -------------------------------
        movea.l graphics_base,a6
        jsr     LVO_WaitTOF(a6)

        bra     main_loop

        ; ------------------------------------------------------------------
        ; Close the window and clean up
        ; ------------------------------------------------------------------
do_close:
        movea.l intuition_base,a6
        movea.l window,a0
        jsr     LVO_CloseWindow(a6)

close_graphics:
        movea.l exec_base,a6
        movea.l graphics_base,a1
        jsr     LVO_CloseLibrary(a6)

close_intuition:
        movea.l exec_base,a6
        movea.l intuition_base,a1
        jsr     LVO_CloseLibrary(a6)

close_exec:
        ; exec.library is never closed.

exit:
        movea.l #DOS_BASE,a6
        jsr     LVO_DOS_Exit(a6)
        bra     exit

; ---------------------------------------------------------------------------
; render_bars - update bar positions and draw them with RectFill.
;
; Register usage during the loop:
;   A2  = pointer into bars array
;   A3  = graphics_base (library base for LVO calls)
;   A4  = window RastPort
;   D5  = inner_width - 1  (xMax for RectFill)
;   D6  = inner_height     (bounce limit)
;   D7  = loop counter (BAR_COUNT-1 .. 0)
;
; Per-bar temporary registers (not clobbered by SetAPen/RectFill glue):
;   D0  = bar position (top y)
;   D1  = bar velocity
;   D2  = bar height
;   D3  = bar pen
;   D4  = scratch for bounce calculation
; ---------------------------------------------------------------------------
render_bars:
        movem.l d2-d7/a2-a4,-(sp)

        movea.l graphics_base,a3
        movea.l rport,a4
        lea     bars,a2
        move.w  win_height,d6          ; inner height (bounce limit)
        move.w  win_width,d5
        subq.w  #1,d5                  ; xMax = inner_width - 1
        moveq   #BAR_COUNT-1,d7        ; loop counter

        ; --- 1. Clear the content area to black -------------------------
        movea.l a4,a1
        moveq   #PEN_BLACK,d0
        movea.l a3,a6
        jsr     LVO_SetAPen(a6)

        movea.l a4,a1
        moveq   #0,d0                  ; xMin
        moveq   #0,d1                  ; yMin
        move.w  d5,d2                  ; xMax
        move.w  d6,d3
        subq.w  #1,d3                  ; yMax = inner_height - 1
        jsr     LVO_RectFill(a6)

        ; --- 2. Draw each bar -------------------------------------------
bar_draw:
        ; Load bar fields from the bars table
        move.w  (a2)+,d0              ; d0 = position (top y)
        move.w  (a2)+,d1              ; d1 = velocity
        move.w  (a2)+,d2              ; d2 = height
        move.w  (a2)+,d3              ; d3 = pen

        ; Update position
        add.w   d1,d0                 ; position += velocity

        ; Bounce off top (keep at least 2 px from the top edge)
        cmp.w   #2,d0
        bge.s   check_bot
        move.w  #2,d0
        neg.w   d1

check_bot:
        ; Bounce off bottom (keep the bar fully visible)
        move.w  d0,d4
        add.w   d2,d4                 ; d4 = bottom y = position + height
        cmp.w   d6,d4                 ; compare with inner_height
        blt.s   store_bar
        move.w  d6,d0
        sub.w   d2,d0                 ; position = inner_height - height
        neg.w   d1

store_bar:
        ; Write back updated position and velocity
        move.w  d0,-8(a2)
        move.w  d1,-6(a2)

        ; Save position and height on the stack (2 bytes each)
        move.w  d0,-(sp)              ; push position
        move.w  d2,-(sp)              ; push height

        ; SetAPen(rp, pen) — A1=rp, D0=pen
        movea.l a4,a1
        move.w  d3,d0
        movea.l a3,a6
        jsr     LVO_SetAPen(a6)

        ; RectFill(rp, 0, position, xMax, position+height-1)
        ;   A1=rp, D0=xMin, D1=yMin, D2=xMax, D3=yMax
        movea.l a4,a1
        moveq   #0,d0                 ; xMin = 0
        move.w  2(sp),d1              ; yMin = position (pushed first, at +2)
        move.w  d5,d2                 ; xMax = inner_width - 1
        move.w  (sp),d3               ; height (pushed last, at +0)
        add.w   d1,d3                 ; yMax = position + height
        subq.w  #1,d3                 ; yMax = position + height - 1
        jsr     LVO_RectFill(a6)

        ; Pop saved values
        addq.l  #4,sp

        ; Next bar
        dbf     d7,bar_draw

        movem.l (sp)+,d2-d7/a2-a4
        rts

; ---------------------------------------------------------------------------
; Data section
; ---------------------------------------------------------------------------
        section data,data

libname_exec:      dc.b "exec.library",0
        even
libname_intuition: dc.b "intuition.library",0
        even
libname_graphics:  dc.b "graphics.library",0
        even
win_title:         dc.b "Copper Bars - Close to exit",0
        even

; Bar state: position, velocity, height, pen (one bar per row).
; Pens use the fixed 16-colour Amiga palette.
bars:
        dc.w  20, 1,16,PEN_RED       ; red
        dc.w  50,-1,20,PEN_GREEN     ; green
        dc.w  80, 2,14,PEN_BLUE      ; blue
        dc.w 110,-2,18,PEN_YELLOW    ; yellow
        dc.w 140, 1,12,PEN_MAGENTA   ; magenta
        dc.w 170,-1,22,PEN_CYAN      ; cyan

        end
