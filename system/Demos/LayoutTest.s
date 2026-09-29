; LayoutTest.s — BOOPSI layout.gadget demo for UAOS (UAOS-128)
;
; Builds a ReAction-style object tree with NewObjectA():
;
;   layout.gadget (vertical, labelled group box)
;     +-- hgroup.gadget (CHILD_WeightedHeight=1)
;     |     +-- button "North"   CHILD_WeightedWidth=1
;     |     +-- button "East"    CHILD_WeightedWidth=1
;     |     +-- button "West"    CHILD_WeightedWidth=1
;     +-- strgclass string       CHILD_Label "Name:"
;     +-- button "Toggle me"     GA_ToggleSelect (checkbox)
;     +-- button "Quit"
;
; The window's FirstGadget list contains only the root layout gadget via
; WA_Gadgets; the kernel splices the child objects in and assigns their
; LeftEdge/TopEdge/Width/Height from the uitree layout engine.  Resizing
; the window reflows automatically (IDCMP_NEWSIZE needs no action).
;
; Pressing a gadget posts IDCMP_GADGETUP whose IAddress carries the child
; object — the demo reads its GadgetID and echoes it in the window title.
;
; Assemble: vasmm68k_mot -Fhunk -o LayoutTest.o LayoutTest.s
; Link:     vlink -bamigahunk -o LayoutTest.hunk LayoutTest.o
;
; ---------------------------------------------------------------------------
; Library bases returned by OpenLibrary
; ---------------------------------------------------------------------------
EXEC_BASE       equ $00000300
DOS_BASE        equ $00000800

; ---------------------------------------------------------------------------
; Exec LVOs
; ---------------------------------------------------------------------------
LVO_OpenLibrary  equ -552
LVO_CloseLibrary equ -414
LVO_GetMsg       equ -372
LVO_ReplyMsg     equ -378
LVO_WaitPort     equ -384

; ---------------------------------------------------------------------------
; Intuition LVOs
; ---------------------------------------------------------------------------
LVO_OpenWindowTags  equ -606
LVO_CloseWindow     equ -72
LVO_AddGList        equ -438
LVO_SetWindowTitles equ -276
LVO_NewObjectA      equ -636
LVO_DisposeObject   equ -642

; ---------------------------------------------------------------------------
; DOS LVO
; ---------------------------------------------------------------------------
LVO_DOS_Exit        equ -144

; ---------------------------------------------------------------------------
; Tags
; ---------------------------------------------------------------------------
TAG_DONE            equ $00000000
TAG_USER            equ $80000000

WA_Dummy            equ (TAG_USER+99)
WA_Left             equ (WA_Dummy+$01)
WA_Top              equ (WA_Dummy+$02)
WA_Width            equ (WA_Dummy+$03)
WA_Height           equ (WA_Dummy+$04)
WA_IDCMP            equ (WA_Dummy+$07)
WA_Gadgets          equ (WA_Dummy+$09)
WA_Title            equ (WA_Dummy+$0B)
WA_MinWidth         equ (WA_Dummy+$0F)
WA_MinHeight        equ (WA_Dummy+$10)
WA_SizeGadget       equ (WA_Dummy+$1E)
WA_DragBar          equ (WA_Dummy+$1F)
WA_DepthGadget      equ (WA_Dummy+$20)
WA_CloseGadget      equ (WA_Dummy+$21)
WA_Activate         equ (WA_Dummy+$26)

GA_Dummy            equ (TAG_USER+$30000)
GA_ID               equ (GA_Dummy+$12)
GA_Text             equ (GA_Dummy+$09)
GA_ToggleSelect     equ (GA_Dummy+$27)
GA_RelVerify        equ (GA_Dummy+$25)

LAYOUT_Dummy        equ (TAG_USER+$40120)
LAYOUT_Orientation  equ (LAYOUT_Dummy+$01)
LAYOUT_AddChild     equ (LAYOUT_Dummy+$02)
LAYOUT_InnerSpacing equ (LAYOUT_Dummy+$05)
LAYOUT_SpaceInner   equ (LAYOUT_Dummy+$06)
LAYOUT_Label        equ (LAYOUT_Dummy+$08)
LAYOUT_BevelState   equ (LAYOUT_Dummy+$09)
LAYOUT_ORIENT_VERT  equ 0

CHILD_Dummy         equ (TAG_USER+$40130)
CHILD_WeightedWidth  equ (CHILD_Dummy+$01)
CHILD_WeightedHeight equ (CHILD_Dummy+$02)
CHILD_Label         equ (CHILD_Dummy+$07)

STRINGA_Dummy       equ (TAG_USER+$40090)
STRINGA_TextVal     equ (STRINGA_Dummy+$01)
STRINGA_MaxChars    equ (STRINGA_Dummy+$02)

; ---------------------------------------------------------------------------
; IDCMP / message offsets
; ---------------------------------------------------------------------------
IDCMP_NEWSIZE       equ $00000002
IDCMP_GADGETDOWN    equ $00000020
IDCMP_GADGETUP      equ $00000040
IDCMP_CLOSEWINDOW   equ $00000100

IM_OFF_CLASS        equ 24
IM_OFF_IADDRESS     equ 32
GAD_OFF_GADGETID    equ 38
WIN_OFF_USERPORT    equ 86

; Gadget IDs assigned to the children
GID_NORTH   equ 101
GID_EAST    equ 102
GID_WEST    equ 103
GID_STRING  equ 104
GID_CHECK   equ 105
GID_QUIT    equ 106

        section bss,bss

exec_base:      ds.l 1
intuition_base: ds.l 1
window:         ds.l 1
userport:       ds.l 1

obj_btn_north:  ds.l 1
obj_btn_east:   ds.l 1
obj_btn_west:   ds.l 1
obj_string:     ds.l 1
obj_check:      ds.l 1
obj_quit:       ds.l 1
obj_hgroup:     ds.l 1
obj_layout:     ds.l 1

        section code,code

; ---------------------------------------------------------------------------
; start — MUST be first: the hunk loader enters at code-hunk offset 0
; ---------------------------------------------------------------------------
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

        ; --- Create the leaf gadgets ------------------------------------
        ; Each NewObjectA call takes (classPtr=0, classID, taglist).
        ; Leaf taglists are static; container taglists are built on the
        ; stack because they must embed the child object pointers.

        ; button "North"
        suba.l  a0,a0
        move.l  #cls_button,a1
        move.l  #tags_north,a2
        bsr     newobj
        move.l  d0,obj_btn_north
        beq.w   close_intuition

        ; button "East"
        move.l  #cls_button,a1
        move.l  #tags_east,a2
        bsr     newobj
        move.l  d0,obj_btn_east
        beq.w   dispose_all

        ; button "West"
        move.l  #cls_button,a1
        move.l  #tags_west,a2
        bsr     newobj
        move.l  d0,obj_btn_west
        beq.w   dispose_all

        ; string gadget
        move.l  #cls_string,a1
        move.l  #tags_string,a2
        bsr     newobj
        move.l  d0,obj_string
        beq.w   dispose_all

        ; checkbox (toggle bool gadget)
        move.l  #cls_button,a1
        move.l  #tags_check,a2
        bsr     newobj
        move.l  d0,obj_check
        beq.w   dispose_all

        ; Quit button
        move.l  #cls_button,a1
        move.l  #tags_quit,a2
        bsr     newobj
        move.l  d0,obj_quit
        beq.w   dispose_all

        ; --- hgroup.gadget with three weighted buttons ------------------
        ; Stack taglist (pushed in reverse order).
        move.l  #0,-(sp)                        ; TAG_DONE pair
        move.l  #TAG_DONE,-(sp)
        move.l  #1,-(sp)                        ; CHILD_WeightedWidth
        move.l  #CHILD_WeightedWidth,-(sp)
        move.l  obj_btn_west,-(sp)              ; LAYOUT_AddChild = west
        move.l  #LAYOUT_AddChild,-(sp)
        move.l  #1,-(sp)
        move.l  #CHILD_WeightedWidth,-(sp)
        move.l  obj_btn_east,-(sp)              ; LAYOUT_AddChild = east
        move.l  #LAYOUT_AddChild,-(sp)
        move.l  #1,-(sp)
        move.l  #CHILD_WeightedWidth,-(sp)
        move.l  obj_btn_north,-(sp)             ; LAYOUT_AddChild = north
        move.l  #LAYOUT_AddChild,-(sp)
        movea.l sp,a2
        move.l  #cls_hgroup,a1
        bsr     newobj
        lea     56(sp),sp                       ; 7 tag pairs
        move.l  d0,obj_hgroup
        beq.w   dispose_all

        ; --- root layout.gadget (vertical, labelled group) --------------
        move.l  #0,-(sp)                        ; TAG_DONE
        move.l  #TAG_DONE,-(sp)
        move.l  obj_quit,-(sp)                  ; LAYOUT_AddChild = quit
        move.l  #LAYOUT_AddChild,-(sp)
        move.l  obj_check,-(sp)                 ; LAYOUT_AddChild = check
        move.l  #LAYOUT_AddChild,-(sp)
        move.l  #str_name,-(sp)                 ; CHILD_Label = "Name:"
        move.l  #CHILD_Label,-(sp)
        move.l  obj_string,-(sp)                ; LAYOUT_AddChild = string
        move.l  #LAYOUT_AddChild,-(sp)
        move.l  #1,-(sp)                        ; CHILD_WeightedHeight
        move.l  #CHILD_WeightedHeight,-(sp)
        move.l  obj_hgroup,-(sp)                ; LAYOUT_AddChild = hgroup
        move.l  #LAYOUT_AddChild,-(sp)
        move.l  #1,-(sp)                        ; LAYOUT_BevelState
        move.l  #LAYOUT_BevelState,-(sp)
        move.l  #str_groups,-(sp)               ; LAYOUT_Label
        move.l  #LAYOUT_Label,-(sp)
        move.l  #4,-(sp)                        ; LAYOUT_InnerSpacing
        move.l  #LAYOUT_InnerSpacing,-(sp)
        move.l  #6,-(sp)                        ; LAYOUT_SpaceInner
        move.l  #LAYOUT_SpaceInner,-(sp)
        move.l  #LAYOUT_ORIENT_VERT,-(sp)       ; LAYOUT_Orientation
        move.l  #LAYOUT_Orientation,-(sp)
        movea.l sp,a2
        move.l  #cls_layout,a1
        bsr     newobj
        lea     96(sp),sp                       ; 12 tag pairs
        move.l  d0,obj_layout
        beq.w   dispose_all

        ; --- OpenWindowTags with WA_Gadgets = root layout ---------------
        suba.l  a0,a0
        move.l  #0,-(sp)                        ; TAG_DONE
        move.l  #TAG_DONE,-(sp)
        move.l  obj_layout,-(sp)                ; WA_Gadgets
        move.l  #WA_Gadgets,-(sp)
        move.l  #160,-(sp)                      ; WA_MinHeight
        move.l  #WA_MinHeight,-(sp)
        move.l  #240,-(sp)                      ; WA_MinWidth
        move.l  #WA_MinWidth,-(sp)
        move.l  #1,-(sp)                        ; WA_Activate
        move.l  #WA_Activate,-(sp)
        move.l  #1,-(sp)                        ; WA_SizeGadget
        move.l  #WA_SizeGadget,-(sp)
        move.l  #1,-(sp)                        ; WA_CloseGadget
        move.l  #WA_CloseGadget,-(sp)
        move.l  #1,-(sp)                        ; WA_DepthGadget
        move.l  #WA_DepthGadget,-(sp)
        move.l  #1,-(sp)                        ; WA_DragBar
        move.l  #WA_DragBar,-(sp)
        move.l  #win_title,-(sp)                ; WA_Title
        move.l  #WA_Title,-(sp)
        move.l  #(IDCMP_CLOSEWINDOW|IDCMP_GADGETUP|IDCMP_GADGETDOWN|IDCMP_NEWSIZE),-(sp)
        move.l  #WA_IDCMP,-(sp)
        move.l  #250,-(sp)                      ; WA_Height
        move.l  #WA_Height,-(sp)
        move.l  #380,-(sp)                      ; WA_Width
        move.l  #WA_Width,-(sp)
        move.l  #110,-(sp)                      ; WA_Top
        move.l  #WA_Top,-(sp)
        move.l  #220,-(sp)                      ; WA_Left
        move.l  #WA_Left,-(sp)
        movea.l intuition_base,a6
        jsr     LVO_OpenWindowTags(a6)
        lea     112(sp),sp                      ; 14 tag pairs
        move.l  d0,window
        beq.w   dispose_all

        movea.l window,a0
        move.l  WIN_OFF_USERPORT(a0),userport

; ---------------------------------------------------------------------------
; Event loop — GADGETUP echoes the pressed gadget's ID in the title bar.
; ---------------------------------------------------------------------------
wait_loop:
        movea.l exec_base,a6
        movea.l userport,a0
        jsr     LVO_WaitPort(a6)
        movea.l exec_base,a6
        movea.l userport,a0
        jsr     LVO_GetMsg(a6)
        tst.l   d0
        beq.s   wait_loop

        movea.l d0,a2                   ; a2 = IntuiMessage
        move.l  IM_OFF_CLASS(a2),d7     ; d7 = IDCMP class
        move.l  IM_OFF_IADDRESS(a2),d6  ; d6 = gadget object (GADGETUP)

        ; ReplyMsg before touching the fields again (message may be freed)
        movea.l a2,a1
        movea.l exec_base,a6
        jsr     LVO_ReplyMsg(a6)

        move.l  d7,d5
        and.l   #IDCMP_CLOSEWINDOW,d5
        bne.w   close_window

        move.l  d7,d5
        and.l   #IDCMP_GADGETUP,d5
        beq.s   wait_loop

        ; --- GADGETUP: IAddress carried the child gadget object ---------
        movea.l d6,a0
        move.w  GAD_OFF_GADGETID(a0),d0 ; gadget ID

        cmp.w   #GID_QUIT,d0
        beq.w   close_window

        ; pick the feedback title for the pressed gadget
        lea     str_pressed_generic,a3
        cmp.w   #GID_NORTH,d0
        bne.s   .not_north
        lea     str_pressed_north,a3
.not_north:
        cmp.w   #GID_EAST,d0
        bne.s   .not_east
        lea     str_pressed_east,a3
.not_east:
        cmp.w   #GID_WEST,d0
        bne.s   .not_west
        lea     str_pressed_west,a3
.not_west:
        cmp.w   #GID_STRING,d0
        bne.s   .not_str
        lea     str_pressed_str,a3
.not_str:
        cmp.w   #GID_CHECK,d0
        bne.s   .set_title
        lea     str_pressed_chk,a3
.set_title:
        movea.l window,a0
        movea.l a3,a1
        suba.l  a2,a2
        movea.l intuition_base,a6
        jsr     LVO_SetWindowTitles(a6)
        bra     wait_loop

; ---------------------------------------------------------------------------
; Cleanup
; ---------------------------------------------------------------------------
close_window:
        movea.l intuition_base,a6
        movea.l window,a0
        jsr     LVO_CloseWindow(a6)

dispose_all:
        movea.l intuition_base,a6
        movea.l obj_layout,a0
        jsr     LVO_DisposeObject(a6)
        movea.l obj_hgroup,a0
        jsr     LVO_DisposeObject(a6)
        movea.l obj_quit,a0
        jsr     LVO_DisposeObject(a6)
        movea.l obj_check,a0
        jsr     LVO_DisposeObject(a6)
        movea.l obj_string,a0
        jsr     LVO_DisposeObject(a6)
        movea.l obj_btn_west,a0
        jsr     LVO_DisposeObject(a6)
        movea.l obj_btn_east,a0
        jsr     LVO_DisposeObject(a6)
        movea.l obj_btn_north,a0
        jsr     LVO_DisposeObject(a6)

close_intuition:
        movea.l exec_base,a6
        movea.l intuition_base,a1
        jsr     LVO_CloseLibrary(a6)
exit:
        movea.l #DOS_BASE,a6
        jsr     LVO_DOS_Exit(a6)
        bra     exit

; ---------------------------------------------------------------------------
; newobj(class_name in a1, taglist in a2) -> object in d0
; ---------------------------------------------------------------------------
newobj:
        suba.l  a0,a0
        movea.l intuition_base,a6
        jsr     LVO_NewObjectA(a6)
        rts

; ---------------------------------------------------------------------------
; Data
; ---------------------------------------------------------------------------
        section data,data

libname_exec:       dc.b "exec.library",0
        even
libname_intuition:  dc.b "intuition.library",0
        even
cls_button:         dc.b "buttongclass",0
        even
cls_string:         dc.b "strgclass",0
        even
cls_layout:         dc.b "layout.gadget",0
        even
cls_hgroup:         dc.b "hgroup.gadget",0
        even

win_title:          dc.b "Layout.gadget test",0
        even
str_groups:         dc.b "Layout Demo",0
        even
str_north:          dc.b "North",0
        even
str_east:           dc.b "East",0
        even
str_west:           dc.b "West",0
        even
str_quit:           dc.b "Quit",0
        even
str_check:          dc.b "Toggle me",0
        even
str_text:           dc.b "edit me",0
        even
str_name:           dc.b "Name:",0
        even
str_pressed_north:  dc.b "Pressed: NORTH",0
        even
str_pressed_east:   dc.b "Pressed: EAST",0
        even
str_pressed_west:   dc.b "Pressed: WEST",0
        even
str_pressed_str:    dc.b "Pressed: STRING",0
        even
str_pressed_chk:    dc.b "Pressed: CHECK",0
        even
str_pressed_generic: dc.b "Pressed!",0
        even

; Static tag lists for leaf objects.
tags_north:  dc.l GA_ID,GID_NORTH, GA_Text,str_north, TAG_DONE,0
tags_east:   dc.l GA_ID,GID_EAST,  GA_Text,str_east,  TAG_DONE,0
tags_west:   dc.l GA_ID,GID_WEST,  GA_Text,str_west,  TAG_DONE,0
tags_quit:   dc.l GA_ID,GID_QUIT,  GA_Text,str_quit,  TAG_DONE,0
tags_check:  dc.l GA_ID,GID_CHECK, GA_Text,str_check, GA_ToggleSelect,1, TAG_DONE,0
tags_string: dc.l GA_ID,GID_STRING, STRINGA_MaxChars,20, STRINGA_TextVal,str_text, TAG_DONE,0

        end
