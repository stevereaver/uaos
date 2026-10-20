; MenuDemo.s - Custom screen + menu strip test for UAOS
;
; Opens a full-size custom Intuition screen (OpenScreenTagList), opens an
; activate-on-open window on it, and installs a menu strip with a single
; menu ("Project") containing a single item ("Quit", command key 'Q').
; The program then waits on the window's UserPort for IDCMP_MENUPICK and
; exits when "Quit" is picked (a MENUPICK whose Code is not MENUNULL).
; A CLOSEWINDOW message also exits, so the close gadget works too.
;
; The menu structures are static: Menu.MenuName points straight at the
; title string, MenuItem.ItemFill points at an IntuiText (ITEMTEXT flag),
; exactly the layout real Intuition parses on AmigaOS 3.1.
;
; Assemble: vasmm68k_mot -Fhunk -o MenuDemo.o MenuDemo.s
; Link:     vlink -bamigahunk -o MenuDemo.hunk MenuDemo.o
;

; ---------------------------------------------------------------------------
; Exec LVOs (standard AmigaOS offsets)
; ---------------------------------------------------------------------------
LVO_OpenLibrary  equ -552
LVO_CloseLibrary equ -414
LVO_GetMsg       equ -372
LVO_ReplyMsg     equ -378
LVO_WaitPort     equ -384

; ---------------------------------------------------------------------------
; Graphics LVOs
; ---------------------------------------------------------------------------
LVO_Move    equ -240
LVO_Text    equ -60
LVO_SetAPen equ -342

; ---------------------------------------------------------------------------
; Intuition LVOs (AmigaOS 3.1 / V40)
; ---------------------------------------------------------------------------
LVO_OpenScreenTagList equ -612
LVO_CloseScreen       equ -66
LVO_OpenWindowTagList equ -606
LVO_CloseWindow       equ -72
LVO_SetMenuStrip      equ -264
LVO_ClearMenuStrip    equ -54

; ---------------------------------------------------------------------------
; Tag values
; ---------------------------------------------------------------------------
TAG_DONE            equ $00000000
TAG_USER            equ $80000000

; SA_ screen tags (intuition/screens.h)
SA_Dummy            equ (TAG_USER+32)
SA_Left             equ (SA_Dummy+$01)
SA_Top              equ (SA_Dummy+$02)
SA_Width            equ (SA_Dummy+$03)
SA_Height           equ (SA_Dummy+$04)
SA_Depth            equ (SA_Dummy+$05)
SA_Title            equ (SA_Dummy+$08)
SA_Type             equ (SA_Dummy+$0D)
SA_DisplayID        equ (SA_Dummy+$12)

; Display mode keys (graphics/displayinfo.h — the pre-V39 monitor keys that
; real 3.1 programs pass to SA_DisplayID; the monitor resolves them to the
; machine's default mode, e.g. PAL hires = 640x256 on a PAL machine).
HIRES_KEY           equ $00008000

; WA_ window tags (intuition/intuition.h)
WA_Dummy            equ (TAG_USER+99)
WA_Left             equ (WA_Dummy+$01)
WA_Top              equ (WA_Dummy+$02)
WA_Width            equ (WA_Dummy+$03)
WA_Height           equ (WA_Dummy+$04)
WA_IDCMP            equ (WA_Dummy+$07)
WA_Title            equ (WA_Dummy+$0B)
WA_CustomScreen     equ (WA_Dummy+$0D)
WA_DragBar          equ (WA_Dummy+$1F)
WA_DepthGadget      equ (WA_Dummy+$20)
WA_CloseGadget      equ (WA_Dummy+$21)
WA_Activate         equ (WA_Dummy+$26)
WA_SmartRefresh     equ (WA_Dummy+$2A)

; ---------------------------------------------------------------------------
; Screen / window constants
; ---------------------------------------------------------------------------
CUSTOMSCREEN        equ $000F
STDSCREENWIDTH      equ -1          ; standard raster for the display mode
STDSCREENHEIGHT     equ -1

; ---------------------------------------------------------------------------
; IDCMP flags (real AmigaOS bit assignments)
; ---------------------------------------------------------------------------
IDCMP_MENUPICK      equ $00000100
IDCMP_CLOSEWINDOW   equ $00000200

; ---------------------------------------------------------------------------
; Window / IntuiMessage structure offsets
; ---------------------------------------------------------------------------
WIN_OFF_RPORT       equ 50
WIN_OFF_USERPORT    equ 86
IM_OFF_CLASS        equ 20
IM_OFF_CODE         equ 24

; ---------------------------------------------------------------------------
; Menu numbers: MENUNULL terminates the MENUPICK stream.
; ---------------------------------------------------------------------------
MENUNULL            equ $FFFF

; ---------------------------------------------------------------------------
; MenuItem flags (intuition/intuition.h)
; ---------------------------------------------------------------------------
CHECKIT             equ $0001
ITEMTEXT            equ $0002
COMMSEQ             equ $0004
MENUTOGGLE          equ $0008
ITEMENABLED         equ $0010
HIGHCOMP            equ $0040           ; select = complement ItemFill
MENUENABLED         equ $0001

; ---------------------------------------------------------------------------
; RastPort pen indices
; ---------------------------------------------------------------------------
PEN_BLACK   equ 0
PEN_WHITE   equ 1

        section bss,bss

exec_base:      ds.l 1
intuition_base: ds.l 1
graphics_base:  ds.l 1
screen:         ds.l 1
window:         ds.l 1
rport:          ds.l 1
userport:       ds.l 1

        section code,code

start:
        ; --- Open exec.library ------------------------------------------
        move.l  #libname_exec,a1
        moveq   #0,d0
        movea.l 4.w,a6
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

        ; --- OpenScreenTagList ------------------------------------------
        ; A0 = NULL (tags carry everything), A1 = scrtags.
        suba.l  a0,a0
        lea     scrtags,a1
        movea.l intuition_base,a6
        jsr     LVO_OpenScreenTagList(a6)
        move.l  d0,screen
        beq.w   close_graphics

        ; --- Patch the screen pointer into the window tag list ----------
        lea     wintags,a0
find_cust:
        cmpi.l  #WA_CustomScreen,(a0)+
        bne.s   find_cust
        move.l  d0,(a0)                 ; WA_CustomScreen data slot

        ; --- OpenWindowTagList ------------------------------------------
        suba.l  a0,a0
        lea     wintags,a1
        movea.l intuition_base,a6
        jsr     LVO_OpenWindowTagList(a6)
        move.l  d0,window
        beq.w   close_screen

        ; --- SetMenuStrip(window, menu_project) --------------------------
        movea.l window,a0
        lea     menu_project,a1
        movea.l intuition_base,a6
        jsr     LVO_SetMenuStrip(a6)
        tst.l   d0
        beq.w   close_window            ; strip refused -> bail

        ; --- Grab the window RastPort and UserPort ----------------------
        movea.l window,a0
        move.l  WIN_OFF_RPORT(a0),rport
        move.l  WIN_OFF_USERPORT(a0),userport

        ; --- Render a hint line into the window -------------------------
        movea.l rport,a1
        moveq   #PEN_WHITE,d0
        movea.l graphics_base,a6
        jsr     LVO_SetAPen(a6)

        movea.l rport,a1
        moveq   #20,d0
        moveq   #60,d1
        jsr     LVO_Move(a6)

        movea.l rport,a1
        move.l  #hint_str,a0
        moveq   #hint_len,d0
        jsr     LVO_Text(a6)

        ; --- Wait for MENUPICK (non-MENUNULL) or CLOSEWINDOW ------------
wait_loop:
        movea.l exec_base,a6
        movea.l userport,a0
        jsr     LVO_WaitPort(a6)
msg_loop:
        movea.l exec_base,a6
        movea.l userport,a0
        jsr     LVO_GetMsg(a6)
        tst.l   d0
        beq.s   wait_loop
        movea.l d0,a2                  ; a2 = IntuiMessage
        move.l  IM_OFF_CLASS(a2),d4    ; d4 = message class
        move.w  IM_OFF_CODE(a2),d5     ; d5 = message code (menu number)
        movea.l a2,a1
        movea.l exec_base,a6
        jsr     LVO_ReplyMsg(a6)
        move.l  d4,d0
        and.l   #IDCMP_CLOSEWINDOW,d0
        bne.s   quit                   ; close gadget -> exit
        move.l  d4,d0
        and.l   #IDCMP_MENUPICK,d0
        beq.s   msg_loop
        cmpi.w  #MENUNULL,d5
        beq.s   msg_loop               ; stream terminator -> keep waiting
        ; Code is a real menu number: menu 0, item 0 = "Quit" -> exit

        ; --- Cleanup ----------------------------------------------------
quit:
        ; ClearMenuStrip(window)
        movea.l window,a0
        movea.l intuition_base,a6
        jsr     LVO_ClearMenuStrip(a6)
close_window:
        movea.l window,a0
        movea.l intuition_base,a6
        jsr     LVO_CloseWindow(a6)
close_screen:
        movea.l screen,a0
        movea.l intuition_base,a6
        jsr     LVO_CloseScreen(a6)
close_graphics:
        movea.l graphics_base,a1
        movea.l exec_base,a6
        jsr     LVO_CloseLibrary(a6)
close_intuition:
        movea.l intuition_base,a1
        movea.l exec_base,a6
        jsr     LVO_CloseLibrary(a6)
close_exec:
        movea.l exec_base,a1
        movea.l 4.w,a6
        jsr     LVO_CloseLibrary(a6)
exit:
        rts                             ; return address is the DOS Exit stub

; ---------------------------------------------------------------------------
; Data
; ---------------------------------------------------------------------------
        section data,data

        cnop    0,4

; --- struct Menu (30 bytes) ------------------------------------------------
menu_project:
        dc.l    0                       ; NextMenu
        dc.w    0                       ; LeftEdge
        dc.w    0                       ; TopEdge
        dc.w    88                      ; Width  (select box on screen bar)
        dc.w    11                      ; Height
        dc.w    MENUENABLED             ; Flags (word: MENUENABLED=$0001,
                                        ;   $0100 would be MIDRAWN)
        dc.l    str_menu                ; MenuName -> "Project"
        dc.l    mi_quit                 ; FirstItem
        dc.w    0,0,0,0                 ; JazzX, JazzY, BeatX, BeatY (unused)

; --- struct MenuItem (38 bytes incl. UserData longword) ---------------------
mi_quit:
        dc.l    0                       ; NextItem
        dc.w    0                       ; LeftEdge
        dc.w    0                       ; TopEdge
        dc.w    80                      ; Width
        dc.w    11                      ; Height
        dc.w    ITEMTEXT|ITEMENABLED|COMMSEQ|HIGHCOMP   ; Flags
                                        ; HIGHCOMP is required: bits 6-7
                                        ; = 0 mean HIGHIMAGE, which makes
                                        ; real Intuition render SelectFill
                                        ; (NULL -> Image at address 0,
                                        ;  mangled glyphs) on selection.
        dc.l    0                       ; MutualExclude
        dc.l    it_quit                 ; ItemFill -> IntuiText
        dc.l    0                       ; SelectFill
        dc.b    'Q'                     ; Command (Amiga+Q)
        dc.b    0                       ; pad
        dc.l    0                       ; SubItem
        dc.w    MENUNULL                ; NextSelect
        dc.l    0                       ; UserData slot (GadTools-style tail)

; --- struct IntuiText (20 bytes) --------------------------------------------
it_quit:
        dc.b    1                       ; FrontPen
        dc.b    0                       ; BackPen
        dc.b    1                       ; DrawMode = JAM2
        dc.b    0                       ; pad
        dc.w    0                       ; LeftEdge
        dc.w    1                       ; TopEdge
        dc.l    0                       ; ITextFont = NULL (screen font)
        dc.l    str_quit                ; IText -> "Quit"
        dc.l    0                       ; NextText

; --- Tag lists --------------------------------------------------------------
scrtags:
        dc.l    SA_Left,        0
        dc.l    SA_Top,         0
        dc.l    SA_Width,       STDSCREENWIDTH
        dc.l    SA_Height,      STDSCREENHEIGHT
        dc.l    SA_Depth,       2
        dc.l    SA_Type,        CUSTOMSCREEN
        dc.l    SA_DisplayID,   HIRES_KEY   ; classic hires raster (640x256
                                            ; PAL) — the screen switches the
                                            ; display mode like on real hw
        dc.l    SA_Title,       str_scrtitle
        dc.l    TAG_DONE,       0

wintags:
        dc.l    WA_Left,        60
        dc.l    WA_Top,         60
        dc.l    WA_Width,       520
        dc.l    WA_Height,      150
        dc.l    WA_IDCMP,       IDCMP_MENUPICK|IDCMP_CLOSEWINDOW
        dc.l    WA_CustomScreen,0      ; patched with the opened Screen*
        dc.l    WA_Title,       str_wintitle
        dc.l    WA_DragBar,     1
        dc.l    WA_DepthGadget, 1
        dc.l    WA_CloseGadget, 1
        dc.l    WA_Activate,    1
        dc.l    WA_SmartRefresh,1
        dc.l    TAG_DONE,       0

; --- Strings -----------------------------------------------------------------
libname_exec:       dc.b "exec.library",0
        even
libname_intuition:  dc.b "intuition.library",0
        even
libname_graphics:   dc.b "graphics.library",0
        even
str_scrtitle:       dc.b "Menu Demo Screen",0
        even
str_wintitle:       dc.b "Menu Demo",0
        even
str_menu:           dc.b "Project",0
        even
str_quit:           dc.b "Quit",0
        even
hint_str:           dc.b "Right mouse button -> Project -> Quit"
hint_len            equ *-hint_str
        even

        end
