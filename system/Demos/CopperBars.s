; CopperBars.s - Amiga Copper Bars demo for UAOS
;
; Opens an Intuition window and renders animated horizontal copper bars
; using the Amiga Copper. The demo can be closed via the window's close
; gadget.
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
LVO_AllocMem     equ -198
LVO_FreeMem      equ -210
LVO_GetMsg       equ -372

; ---------------------------------------------------------------------------
; Graphics LVOs (UAOS project slot mapping: LVO = -slot * 6)
; ---------------------------------------------------------------------------
LVO_InitView     equ -360   ; slot 60
LVO_InitVPort    equ -204   ; slot 34
LVO_MakeVPort    equ -216   ; slot 36
LVO_MrgCop       equ -210   ; slot 35
LVO_LoadView     equ -222   ; slot 37
LVO_InitBitMap   equ -390   ; slot 65
LVO_InitRastPort equ -198   ; slot 33
LVO_LoadRGB4     equ -192   ; slot 32
LVO_GetColorMap  equ -570   ; slot 95
LVO_FreeColorMap equ -576   ; slot 96
LVO_AllocBitMap  equ -918   ; slot 153
LVO_FreeBitMap   equ -924   ; slot 154
LVO_BltClear     equ -300   ; slot 50
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
; Memory flags
; ---------------------------------------------------------------------------
MEMF_PUBLIC     equ $00000001
MEMF_CHIP       equ $00000002
MEMF_FAST       equ $00000004
MEMF_CLEAR      equ $00010000

; ---------------------------------------------------------------------------
; Amiga custom chip registers
; ---------------------------------------------------------------------------
CUSTOM_BASE     equ $DFF000
COP1LC          equ $080
COPJMP1         equ $088
COLOR00         equ $180

; ---------------------------------------------------------------------------
; IDCMP and window flags
; ---------------------------------------------------------------------------
IDCMP_CLOSEWINDOW equ $00000200

WFLG_CLOSEGADGET  equ $00000040
WFLG_DRAGBAR      equ $00000002
WFLG_DEPTHGADGET  equ $00000004
WFLG_ACTIVATE     equ $00001000
WFLG_NOCAREREFRESH equ $00002000

; ---------------------------------------------------------------------------
; Structure offsets
; ---------------------------------------------------------------------------
VIEW_VIEWPORT   equ 0
VIEW_SIZE       equ 16

VP_NEXT         equ 0
VP_RASINFO      equ 14
VP_COLORMAP     equ 18
VP_DWIDTH       equ 22
VP_DHEIGHT      equ 24
VP_DXOFFSET     equ 26
VP_DYOFFSET     equ 28
VP_MODES        equ 30
VP_SIZE         equ 40

RI_BITMAP       equ 0
RI_NEXT         equ 4
RI_RXOFFSET     equ 8
RI_RYOFFSET     equ 10
RI_SIZE         equ 12

WIN_OFF_USERPORT equ 86

IM_OFF_CLASS    equ 24

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
; Demo parameters
; ---------------------------------------------------------------------------
BAR_COUNT       equ 6
SCREEN_HEIGHT   equ 256

        section bss,bss

exec_base:      ds.l 1
intuition_base: ds.l 1
graphics_base:  ds.l 1
window:         ds.l 1
view:           ds.l 1
viewport:       ds.l 1
rasinfo:        ds.l 1
bitmap:         ds.l 1
colormap:       ds.l 1
copper_addr:    ds.l 1
newwindow:      ds.b 44

        section code,code

start:
        ; Open exec.library - this is a no-op in UAOS but sets the base.
        move.l	#libname_exec,a1
        moveq   #0,d0
        movea.l #EXEC_BASE,a6
        jsr     LVO_OpenLibrary(a6)
        movea.l d0,a6
        beq.w   exit
        move.l  a6,exec_base

        ; Open intuition.library
        move.l	#libname_intuition,a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,intuition_base
        beq.w   close_exec

        ; Open graphics.library
        move.l	#libname_graphics,a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,graphics_base
        beq.w   close_intuition

        ; Allocate View, ViewPort, RasInfo, ColorMap, and NewWindow.
        move.l  #VIEW_SIZE+VP_SIZE+RI_SIZE+44,d0
        or.l    #MEMF_CLEAR,d0
        moveq   #MEMF_FAST,d1
        jsr     LVO_AllocMem(a6)
        move.l  d0,view
        beq.w   close_graphics

        ; Set up the linked structure pointers.
        move.l  view,d0
        add.l   #VIEW_SIZE,d0
        move.l  d0,viewport
        add.l   #VP_SIZE,d0
        move.l  d0,rasinfo
        add.l   #RI_SIZE,d0
        move.l  d0,newwindow

        ; Allocate a 1-bitplane dummy BitMap for MakeVPort.
        movea.l graphics_base,a6
        move.l  #320,d0
        move.l  #256,d1
        moveq   #1,d2
        moveq   #0,d3
        suba.l  a0,a0
        jsr     LVO_AllocBitMap(a6)
        move.l  d0,bitmap
        beq.w   cleanup_mem

        ; Allocate a ColorMap.
        moveq   #32,d0
        jsr     LVO_GetColorMap(a6)
        move.l  d0,colormap
        beq.w   cleanup_bitmap

        ; Initialize View and ViewPort.
        movea.l view,a1
        jsr     LVO_InitView(a6)
        movea.l viewport,a1
        jsr     LVO_InitVPort(a6)

        ; Fill in ViewPort fields.
        movea.l viewport,a0
        move.w  #320,VP_DWIDTH(a0)
        move.w  #256,VP_DHEIGHT(a0)
        move.w  #0,VP_DXOFFSET(a0)
        move.w  #0,VP_DYOFFSET(a0)
        move.l  #0,VP_MODES(a0)
        move.l  #0,VP_NEXT(a0)
        move.l  rasinfo,VP_RASINFO(a0)
        move.l  colormap,VP_COLORMAP(a0)

        ; Fill in RasInfo.
        movea.l rasinfo,a0
        move.l  bitmap,RI_BITMAP(a0)
        move.l  #0,RI_NEXT(a0)
        move.w  #0,RI_RXOFFSET(a0)
        move.w  #0,RI_RYOFFSET(a0)

        ; Link ViewPort into the View.
        movea.l view,a0
        move.l  viewport,VIEW_VIEWPORT(a0)

        ; Load initial palette.
        movea.l viewport,a0
        move.l	#colors,a1
        moveq   #16,d0
        jsr     LVO_LoadRGB4(a6)

        ; Build the initial copper list via the graphics library.
        movea.l view,a0
        movea.l viewport,a1
        jsr     LVO_MakeVPort(a6)
        movea.l view,a1
        jsr     LVO_MrgCop(a6)
        movea.l view,a1
        jsr     LVO_LoadView(a6)

        ; Read back the merged copper list address from COP1LC.
        movea.l #CUSTOM_BASE,a0
        move.l  COP1LC(a0),d0
        move.l  d0,copper_addr

        ; Build the initial custom copper bar list.
        bsr     build_copper

        ; Re-render with the custom copper list.
        movea.l view,a1
        jsr     LVO_LoadView(a6)

        ; Open the Intuition window.
        movea.l exec_base,a6
        movea.l newwindow,a0
        move.w  #120,NW_LEFTEDGE(a0)
        move.w  #60,NW_TOPEDGE(a0)
        move.w  #320,NW_WIDTH(a0)
        move.w  #80,NW_HEIGHT(a0)
        move.b  #1,NW_DETAILPEN(a0)
        move.b  #0,NW_BLOCKPEN(a0)
        move.w  #IDCMP_CLOSEWINDOW,NW_IDCMPFLAGS(a0)
        move.l  #(WFLG_CLOSEGADGET|WFLG_DRAGBAR|WFLG_DEPTHGADGET|WFLG_ACTIVATE|WFLG_NOCAREREFRESH),NW_FLAGS(a0)
        clr.l   NW_FIRSTGADGET(a0)
        clr.l   NW_CHECKMARK(a0)
        move.l	#win_title,a1
        move.l  a1,NW_TITLE(a0)
        clr.l   NW_SCREEN(a0)
        clr.l   NW_BITMAP(a0)
        move.w  #120,NW_MINWIDTH(a0)
        move.w  #60,NW_MINHEIGHT(a0)
        move.w  #640,NW_MAXWIDTH(a0)
        move.w  #400,NW_MAXHEIGHT(a0)
        clr.w   NW_TYPE(a0)

        movea.l intuition_base,a6
        movea.l newwindow,a0
        jsr     LVO_OpenWindow(a6)
        move.l  d0,window
        beq.w   cleanup_all

        ; Ensure the window reports close events.
        movea.l window,a0
        move.l  #IDCMP_CLOSEWINDOW,d0
        jsr     LVO_ModifyIDCMP(a6)

        ; ------------------------------------------------------------------
        ; Main animation loop.
        ; ------------------------------------------------------------------
main_loop:
        ; Check for a close-window IDCMP message.
        movea.l exec_base,a6
        movea.l window,a0
        movea.l WIN_OFF_USERPORT(a0),a0
        jsr     LVO_GetMsg(a6)
        tst.l   d0
        beq.s   no_msg
        movea.l d0,a0
        move.l  IM_OFF_CLASS(a0),d0
        and.l   #IDCMP_CLOSEWINDOW,d0
        bne.w   do_close
no_msg:
        ; Update the copper list and re-render.
        bsr     update_copper

        movea.l graphics_base,a6
        movea.l view,a1
        jsr     LVO_LoadView(a6)

        ; Pace the animation at ~50 Hz.
        jsr     LVO_WaitTOF(a6)

        bra     main_loop

        ; ------------------------------------------------------------------
        ; Close the window and clean up.
        ; ------------------------------------------------------------------
do_close:
        movea.l intuition_base,a6
        movea.l window,a0
        jsr     LVO_CloseWindow(a6)

cleanup_all:
        movea.l graphics_base,a6
        movea.l colormap,a0
        jsr     LVO_FreeColorMap(a6)

cleanup_bitmap:
        movea.l graphics_base,a6
        movea.l bitmap,a0
        jsr     LVO_FreeBitMap(a6)

cleanup_mem:
        movea.l exec_base,a6
        movea.l view,a1
        move.l  #VIEW_SIZE+VP_SIZE+RI_SIZE+44,d0
        jsr     LVO_FreeMem(a6)

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
; build_copper - write the custom copper bar list to copper_addr.
; Uses the current bar positions from the data section.
; ---------------------------------------------------------------------------
build_copper:
        movea.l copper_addr,a1
        move.l	#bars,a2
        moveq   #BAR_COUNT-1,d7

        ; Initial background color: black.
        move.w  #$0180,(a1)+
        move.w  #$0000,(a1)+

bar_loop:
        move.w  (a2)+,d0           ; d0 = top
        move.w  (a2)+,d1           ; d1 = velocity (skip)
        move.w  (a2)+,d2           ; d2 = height
        move.w  (a2)+,d3           ; d3 = color
        move.w  d0,d4
        add.w   d2,d4              ; d4 = bottom

        ; WAIT top, HP=0, mask=VP=all HP=ignore
        move.w  d0,d1
        lsl.w   #8,d1
        or.w    #$0001,d1
        move.w  d1,(a1)+
        move.w  #$FF01,(a1)+

        ; MOVE COLOR00, color
        move.w  #$0180,(a1)+
        move.w  d3,(a1)+

        ; WAIT bottom
        move.w  d4,d1
        lsl.w   #8,d1
        or.w    #$0001,d1
        move.w  d1,(a1)+
        move.w  #$FF01,(a1)+

        ; MOVE COLOR00, black
        move.w  #$0180,(a1)+
        move.w  #$0000,(a1)+

        dbf     d7,bar_loop

        ; End-of-copper-list: WAIT $FFFE,$FFFE
        move.w  #$FFFE,(a1)+
        move.w  #$FFFE,(a1)+
        rts

; ---------------------------------------------------------------------------
; update_copper - move the bars, bounce them, then rebuild the copper list.
; ---------------------------------------------------------------------------
update_copper:
        movea.l #bars,a2
        moveq   #BAR_COUNT-1,d7

update_loop:
        move.w  (a2)+,d0           ; d0 = position
        move.w  (a2)+,d1           ; d1 = velocity
        move.w  (a2)+,d2           ; d2 = height
        move.w  (a2)+,d3           ; d3 = color

        add.w   d1,d0              ; position += velocity

        ; Bounce off the top/bottom edges.
        cmp.w   #8,d0
        bge.s   check_bottom
        move.w  #8,d0
        neg.w   d1
check_bottom:
        move.w  d0,d4
        add.w   d2,d4
        cmp.w   #SCREEN_HEIGHT-8,d4
        ble.s   store_bar
        move.w  #SCREEN_HEIGHT-8,d4
        sub.w   d2,d4
        move.w  d4,d0
        neg.w   d1

store_bar:
        ; Write back position and velocity.
        move.w  d0,-8(a2)
        move.w  d1,-6(a2)

        dbf     d7,update_loop

        ; Now rebuild the copper list.
        bra     build_copper

        section data,data

libname_exec:      dc.b "exec.library",0
        even
libname_intuition: dc.b "intuition.library",0
        even
libname_graphics:  dc.b "graphics.library",0
        even
win_title:         dc.b "Copper Bars Demo - Close to exit",0
        even

; Initial 4-bit RGB palette (16 entries) for the ColorMap.
colors:
        dc.w $0000,$00F0,$0F00,$0FF0,$F000,$F0F0,$FF00,$FFFF
        dc.w $0888,$088F,$08F0,$08FF,$0F80,$0F8F,$0FF8,$0FFF

; Bar state: position, velocity, height, color (one bar per line).
bars:
        dc.w  30, 1,16,$0F00   ; red
        dc.w  60,-1,20,$00F0   ; green
        dc.w  90, 2,14,$F000   ; blue
        dc.w 120,-2,18,$0FF0   ; yellow
        dc.w 150, 1,12,$F0F0   ; magenta
        dc.w 180,-1,22,$FF00   ; cyan

        end
