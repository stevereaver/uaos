; AGATest.s - AGA 256-colour graphics capability test for UAOS
;
; Opens a custom Intuition screen and a SuperBitMap window backed by an
; 8-bitplane (AGA) BitMap, installs a 256-entry RGB palette on the
; screen's ViewPort ColorMap, and renders an animated 256-colour diagonal
; plasma into the bitmap via WritePixelArray8 (chunky -> planar).
;
; The 8-bitplane planar bitmap, the 256-entry ColorMap and the chunky
; write path together exercise the emulator's AGA graphics pipeline:
;   * AllocBitMap with depth 8  (8 bitplanes)
;   * GetColorMap(256) + SetRGB32  (256-entry AGA palette)
;   * WritePixelArray8 chunky-to-planar conversion at 8 bitplanes
;   * render_bitmap_to_framebuffer with a 256-entry colour lookup
;
; Any mouse click (or the window close gadget) exits the demo.
;
; Memory is allocated through exec.library AllocMem and graphics.library
; AllocBitMap/GetColorMap, and everything is freed on exit.
;
; Assemble: vasmm68k_mot -Fhunk -o AGATest.o AGATest.s
; Link:     vlink -bamigahunk -o AGATest.hunk AGATest.o
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
LVO_ReplyMsg     equ -378

; ---------------------------------------------------------------------------
; Graphics LVOs (slot = |LVO|/6)
; ---------------------------------------------------------------------------
LVO_AllocBitMap     equ -918    ; slot 153
LVO_FreeBitMap      equ -924    ; slot 154
LVO_GetColorMap     equ -570    ; slot 95
LVO_FreeColorMap    equ -576    ; slot 96
LVO_SetRGB32        equ -852    ; slot 142
LVO_WritePixelArray8 equ -786   ; slot 131
LVO_WaitTOF         equ -270    ; slot 45

; ---------------------------------------------------------------------------
; Intuition LVOs
; ---------------------------------------------------------------------------
LVO_OpenScreenTags  equ -612
LVO_OpenWindowTags  equ -606
LVO_CloseScreen     equ -66
LVO_CloseWindow     equ -72

; ---------------------------------------------------------------------------
; DOS LVO
; ---------------------------------------------------------------------------
LVO_DOS_Exit        equ -144

; ---------------------------------------------------------------------------
; Tag base values
; ---------------------------------------------------------------------------
TAG_DONE            equ $00000000
TAG_USER            equ $80000000

SA_Dummy            equ (TAG_USER+32)
SA_Left             equ (SA_Dummy+$0001)
SA_Top              equ (SA_Dummy+$0002)
SA_Width            equ (SA_Dummy+$0003)
SA_Height           equ (SA_Dummy+$0004)
SA_Depth            equ (SA_Dummy+$0005)
SA_ShowTitle        equ (SA_Dummy+$0016)
SA_Quiet            equ (SA_Dummy+$0018)
SA_ColorMapEntries  equ (SA_Dummy+$001C)

WA_Dummy            equ (TAG_USER+99)
WA_Left             equ (WA_Dummy+$01)
WA_Top              equ (WA_Dummy+$02)
WA_Width            equ (WA_Dummy+$03)
WA_Height           equ (WA_Dummy+$04)
WA_IDCMP            equ (WA_Dummy+$07)
WA_Title            equ (WA_Dummy+$0B)
WA_CustomScreen     equ (WA_Dummy+$0D)
WA_SuperBitMap      equ (WA_Dummy+$0E)
WA_DragBar          equ (WA_Dummy+$1F)
WA_DepthGadget      equ (WA_Dummy+$20)
WA_CloseGadget      equ (WA_Dummy+$21)
WA_Backdrop         equ (WA_Dummy+$22)
WA_Activate         equ (WA_Dummy+$26)

; ---------------------------------------------------------------------------
; IDCMP flags
; ---------------------------------------------------------------------------
IDCMP_MOUSEBUTTONS  equ $00000008
IDCMP_CLOSEWINDOW   equ $00000100

; ---------------------------------------------------------------------------
; Structure offsets
; ---------------------------------------------------------------------------
SCR_OFF_VIEWPORT    equ 46
WIN_OFF_RPORT       equ 50
WIN_OFF_USERPORT    equ 86
IM_OFF_CLASS        equ 24
VP_OFF_COLORMAP     equ 18
RP_OFF_MASK         equ 40

; ---------------------------------------------------------------------------
; BitMap flags
; ---------------------------------------------------------------------------
BMF_CLEAR           equ $00000001

; ---------------------------------------------------------------------------
; Drawing dimensions (the 8-bitplane SuperBitMap)
; ---------------------------------------------------------------------------
BMP_W               equ 320
BMP_H               equ 200
CHUNKY_SIZE         equ (BMP_W*BMP_H)        ; 64000 bytes

SCREEN_W            equ 400
SCREEN_H            equ 300
WIN_W               equ 340
WIN_H               equ 260

        section bss,bss

exec_base:      ds.l 1
intuition_base: ds.l 1
graphics_base:  ds.l 1
screen:         ds.l 1
window:         ds.l 1
rport:          ds.l 1
bitmap:         ds.l 1
colormap:       ds.l 1
viewport:       ds.l 1
chunky:         ds.l 1
pal_i:          ds.l 1

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
        beq.w   exit

        ; --- Open graphics.library --------------------------------------
        move.l  #libname_graphics,a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,graphics_base
        beq.w   close_intuition

        ; --- AllocBitMap(320, 200, 8, BMF_CLEAR, 0) ---------------------
        movea.l graphics_base,a6
        move.l  #BMP_W,d0
        move.l  #BMP_H,d1
        move.l  #8,d2
        move.l  #BMF_CLEAR,d3
        suba.l  a0,a0
        jsr     LVO_AllocBitMap(a6)
        move.l  d0,bitmap
        beq.w   close_graphics

        ; --- GetColorMap(256) -------------------------------------------
        move.l  #256,d0
        jsr     LVO_GetColorMap(a6)
        move.l  d0,colormap
        beq.w   free_bitmap

        ; --- AllocMem(64) for a minimal ViewPort ------------------------
        movea.l exec_base,a6
        move.l  #64,d0
        moveq   #0,d1
        jsr     LVO_AllocMem(a6)
        move.l  d0,viewport
        beq.w   free_colormap
        ; Zero the 64-byte ViewPort and link its ColorMap (offset 18).
        movea.l d0,a0
        move.w  #15,d1
vp_clr:
        clr.l   (a0)+
        dbf     d1,vp_clr
        movea.l viewport,a0
        move.l  colormap,VP_OFF_COLORMAP(a0)

        ; --- AllocMem(CHUNKY_SIZE) for the chunky pixel buffer ----------
        movea.l exec_base,a6
        move.l  #CHUNKY_SIZE,d0
        moveq   #0,d1
        jsr     LVO_AllocMem(a6)
        move.l  d0,chunky
        beq.w   free_viewport

        ; --- OpenScreenTags ---------------------------------------------
        ; A0 = 0 (no NewScreen); all parameters come from the tag list.
        suba.l  a0,a0
        move.l  #0,-(sp)                       ; TAG_DONE data
        move.l  #TAG_DONE,-(sp)                ; TAG_DONE
        move.l  #256,-(sp)                     ; SA_ColorMapEntries data
        move.l  #SA_ColorMapEntries,-(sp)
        move.l  #0,-(sp)                       ; SA_ShowTitle = FALSE
        move.l  #SA_ShowTitle,-(sp)
        move.l  #1,-(sp)                       ; SA_Quiet = TRUE
        move.l  #SA_Quiet,-(sp)
        move.l  #8,-(sp)                       ; SA_Depth = 8 (AGA)
        move.l  #SA_Depth,-(sp)
        move.l  #SCREEN_H,-(sp)                ; SA_Height
        move.l  #SA_Height,-(sp)
        move.l  #SCREEN_W,-(sp)                ; SA_Width
        move.l  #SA_Width,-(sp)
        move.l  #30,-(sp)                      ; SA_Top
        move.l  #SA_Top,-(sp)
        move.l  #40,-(sp)                      ; SA_Left
        move.l  #SA_Left,-(sp)
        movea.l intuition_base,a6
        jsr     LVO_OpenScreenTags(a6)
        lea     72(sp),sp                      ; 9 tag pairs = 72 bytes
        move.l  d0,screen
        beq.w   free_chunky

        ; Link our ViewPort (holding the 256-entry ColorMap) into the screen.
        movea.l screen,a0
        move.l  viewport,SCR_OFF_VIEWPORT(a0)

        ; --- Install the 256-entry AGA palette --------------------------
        bsr     install_palette

        ; --- OpenWindowTags ---------------------------------------------
        suba.l  a0,a0
        move.l  #0,-(sp)                       ; TAG_DONE data
        move.l  #TAG_DONE,-(sp)                ; TAG_DONE
        move.l  #1,-(sp)                       ; WA_Activate = TRUE
        move.l  #WA_Activate,-(sp)
        move.l  #(IDCMP_MOUSEBUTTONS|IDCMP_CLOSEWINDOW),-(sp)
        move.l  #WA_IDCMP,-(sp)
        move.l  bitmap,-(sp)                   ; WA_SuperBitMap = bm
        move.l  #WA_SuperBitMap,-(sp)
        move.l  #1,-(sp)                       ; WA_CloseGadget = TRUE
        move.l  #WA_CloseGadget,-(sp)
        move.l  #1,-(sp)                       ; WA_DepthGadget = TRUE
        move.l  #WA_DepthGadget,-(sp)
        move.l  #1,-(sp)                       ; WA_DragBar = TRUE
        move.l  #WA_DragBar,-(sp)
        pea     win_title                      ; WA_Title
        move.l  #WA_Title,-(sp)
        move.l  #WIN_H,-(sp)                   ; WA_Height
        move.l  #WA_Height,-(sp)
        move.l  #WIN_W,-(sp)                   ; WA_Width
        move.l  #WA_Width,-(sp)
        move.l  #10,-(sp)                      ; WA_Top
        move.l  #WA_Top,-(sp)
        move.l  #10,-(sp)                      ; WA_Left
        move.l  #WA_Left,-(sp)
        move.l  screen,-(sp)                   ; WA_CustomScreen
        move.l  #WA_CustomScreen,-(sp)
        movea.l intuition_base,a6
        jsr     LVO_OpenWindowTags(a6)
        lea     104(sp),sp                     ; 13 tag pairs = 104 bytes
        move.l  d0,window
        beq.w   close_screen

        ; --- Grab the window RastPort and enable all 8 bitplane writes --
        movea.l window,a0
        move.l  WIN_OFF_RPORT(a0),d0
        move.l  d0,rport
        movea.l d0,a0
        move.b  #$FF,RP_OFF_MASK(a0)           ; write mask = all planes

        ; --- Initialise the chunky plasma buffer: pen = (x + y) & $FF ---
        bsr     init_chunky

        ; ------------------------------------------------------------------
        ; Main animation loop
        ; ------------------------------------------------------------------
main_loop:
        ; --- Drain IDCMP messages; exit on mouse button / close ---------
        movea.l exec_base,a6
        movea.l window,a0
        movea.l WIN_OFF_USERPORT(a0),a0
        jsr     LVO_GetMsg(a6)
        tst.l   d0
        beq.s   no_msg
        move.l  d0,a0                  ; a0 = IntuiMessage
        move.l  IM_OFF_CLASS(a0),d1    ; d1 = message class
        move.l  a0,a1                  ; a1 = message for ReplyMsg
        movea.l exec_base,a6
        jsr     LVO_ReplyMsg(a6)
        move.l  d1,d2
        and.l   #IDCMP_MOUSEBUTTONS,d2
        bne.w   do_exit
        move.l  d1,d2
        and.l   #IDCMP_CLOSEWINDOW,d2
        bne.w   do_exit

no_msg:
        ; --- Scroll the plasma by adding 1 to every chunky byte ---------
        lea     chunky,a0
        movea.l (a0),a0
        move.l  #(CHUNKY_SIZE-1),d0
scroll_lp:
        addq.b  #1,(a0)+
        dbf     d0,scroll_lp

        ; --- WritePixelArray8(rp, 0, 0, BMP_W, BMP_H, chunky, BMP_W) ----
        ; A1=rp, D0=x, D1=y, D2=w, D3=h, A0=array, A2=bpr
        movea.l rport,a1
        moveq   #0,d0
        moveq   #0,d1
        move.l  #BMP_W,d2
        move.l  #BMP_H,d3
        move.l  chunky,a0
        move.l  #BMP_W,a2
        movea.l graphics_base,a6
        jsr     LVO_WritePixelArray8(a6)

        ; --- Pace the animation at ~50 Hz -------------------------------
        movea.l graphics_base,a6
        jsr     LVO_WaitTOF(a6)

        bra     main_loop

        ; ------------------------------------------------------------------
        ; Cleanup — close/free everything in reverse order of allocation.
        ; ------------------------------------------------------------------
do_exit:
        ; chunky was allocated: free it, then close the window.
        movea.l exec_base,a6
        movea.l chunky,a1
        move.l  #CHUNKY_SIZE,d0
        jsr     LVO_FreeMem(a6)
close_window:
        movea.l intuition_base,a6
        movea.l window,a0
        jsr     LVO_CloseWindow(a6)
close_screen:
        movea.l intuition_base,a6
        movea.l screen,a0
        jsr     LVO_CloseScreen(a6)
free_chunky:
        movea.l exec_base,a6
        movea.l chunky,a1
        move.l  #CHUNKY_SIZE,d0
        jsr     LVO_FreeMem(a6)
free_viewport:
        movea.l exec_base,a6
        movea.l viewport,a1
        move.l  #64,d0
        jsr     LVO_FreeMem(a6)
free_colormap:
        movea.l graphics_base,a6
        movea.l colormap,a0
        jsr     LVO_FreeColorMap(a6)
free_bitmap:
        movea.l graphics_base,a6
        movea.l bitmap,a1
        jsr     LVO_FreeBitMap(a6)
close_graphics:
        movea.l exec_base,a6
        movea.l graphics_base,a1
        jsr     LVO_CloseLibrary(a6)
close_intuition:
        movea.l exec_base,a6
        movea.l intuition_base,a1
        jsr     LVO_CloseLibrary(a6)
exit:
        movea.l #DOS_BASE,a6
        jsr     LVO_DOS_Exit(a6)
        bra     exit

; ---------------------------------------------------------------------------
; init_chunky - fill the chunky buffer with pen = (x + y) & $FF
; ---------------------------------------------------------------------------
init_chunky:
        movem.l d1-d4/a0,-(sp)
        move.l  chunky,a0
        moveq   #0,d1                  ; d1 = y
        move.w  #(BMP_H-1),d4          ; d4 = row counter
icy_loop:
        move.w  d1,d2                  ; d2 = y (pen for x = 0)
        move.w  #(BMP_W-1),d3          ; d3 = column counter
icx_loop:
        move.b  d2,(a0)+
        addq.b  #1,d2                  ; pen = (x + y + 1) & $FF
        dbf     d3,icx_loop
        addq.w  #1,d1
        dbf     d4,icy_loop
        movem.l (sp)+,d1-d4/a0
        rts

; ---------------------------------------------------------------------------
; install_palette - program 256 RGB entries via SetRGB32(vp, i, r, g, b).
;
; A 4-band rainbow is generated from the pen index i:
;   band = i >> 6        (0..3)
;   f    = (i & $3F) << 2 (0..252)
;   band 0: R=255, G=f,      B=0      (red   -> yellow)
;   band 1: R=255-f, G=255,  B=0      (yellow-> green)
;   band 2: R=0,    G=255,   B=f      (green -> cyan)
;   band 3: R=0,    G=255-f, B=255    (cyan  -> blue)
;
; SetRGB32 takes 32-bit components in the high byte, so each channel is
; shifted left by 24 bits before being passed in D1/D2/D3.
; ---------------------------------------------------------------------------
install_palette:
        movem.l d5-d7/a0/a6,-(sp)
        move.l  #0,pal_i
pal_loop:
        move.l  pal_i,d7              ; d7 = i
        move.l  d7,d6
        lsr.l   #6,d6                 ; d6 = band (0..3)
        move.l  d7,d5
        and.w   #$3F,d5               ; d5 = f (0..63)
        lsl.w   #2,d5                 ; d5 = f*4 (0..252)

        cmp.w   #0,d6
        beq.s   pal_band0
        cmp.w   #1,d6
        beq.s   pal_band1
        cmp.w   #2,d6
        beq.s   pal_band2
        ; fallthrough: band 3
        moveq   #0,d1                 ; R = 0
        moveq   #-1,d2               ; G = 255
        sub.w   d5,d2                 ; G = 255 - f
        moveq   #-1,d3               ; B = 255
        bra.s   pal_set

pal_band0:
        moveq   #-1,d1               ; R = 255
        move.w  d5,d2                 ; G = f
        moveq   #0,d3                 ; B = 0
        bra.s   pal_set

pal_band1:
        moveq   #-1,d1               ; R = 255
        sub.w   d5,d1                 ; R = 255 - f
        moveq   #-1,d2               ; G = 255
        moveq   #0,d3                 ; B = 0
        bra.s   pal_set

pal_band2:
        moveq   #0,d1                 ; R = 0
        moveq   #-1,d2               ; G = 255
        move.w  d5,d3                 ; B = f

pal_set:
        movea.l viewport,a0          ; A0 = ViewPort
        move.l  d7,d0                 ; D0 = index
        lsl.l   #8,d1
        lsl.l   #8,d1
        lsl.l   #8,d1                 ; D1 = R << 24
        lsl.l   #8,d2
        lsl.l   #8,d2
        lsl.l   #8,d2                 ; D2 = G << 24
        lsl.l   #8,d3
        lsl.l   #8,d3
        lsl.l   #8,d3                 ; D3 = B << 24
        movea.l graphics_base,a6
        jsr     LVO_SetRGB32(a6)

        move.l  pal_i,d7              ; reload (callee may clobber d7)
        addq.l  #1,d7
        move.l  d7,pal_i
        cmp.l   #256,d7
        blt.s   pal_loop

        movem.l (sp)+,d5-d7/a0/a6
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
win_title:         dc.b "AGA 256-Colour Test - Click to exit",0
        even

        end
