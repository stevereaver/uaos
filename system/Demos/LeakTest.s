; LeakTest.s — UAOS-265 regression: orphan Intuition objects on task exit
;
; Opens a custom Intuition screen and a window on it, then exits WITHOUT
; calling CloseWindow/CloseScreen/CloseLibrary.  Task teardown must retire
; the window and screen slots; otherwise the released RAM window leaves
; armed guest pointers that the compositor decodes through whatever g_ram
; is bound next (foreign/reused memory -> alien-palette full-screen emit).
;
; Assemble: vasmm68k_mot -Fhunk -o LeakTest.o LeakTest.s
; Link:     vlink -bamigahunk -o LeakTest.hunk LeakTest.o
;

LVO_OpenLibrary    equ -552
LVO_OpenScreenTags equ -612
LVO_OpenWindowTags equ -606

TAG_DONE            equ $00000000
TAG_USER            equ $80000000

SA_Dummy            equ (TAG_USER+32)
SA_ShowTitle        equ (SA_Dummy+$0016)
SA_Quiet            equ (SA_Dummy+$0018)

WA_Dummy            equ (TAG_USER+99)
WA_Left             equ (WA_Dummy+$01)
WA_Top              equ (WA_Dummy+$02)
WA_Width            equ (WA_Dummy+$03)
WA_Height           equ (WA_Dummy+$04)
WA_IDCMP            equ (WA_Dummy+$07)
WA_Title            equ (WA_Dummy+$0B)
WA_CustomScreen     equ (WA_Dummy+$0D)
WA_DragBar          equ (WA_Dummy+$1F)
WA_CloseGadget      equ (WA_Dummy+$21)
WA_Activate         equ (WA_Dummy+$26)

IDCMP_CLOSEWINDOW   equ $00000100

        section code,code

start:
        ; --- Open intuition.library --------------------------------------
        movea.l $4.w,a6
        move.l  #libname_intuition,a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,a5                     ; a5 = intuition_base
        beq.w   exit

        ; --- OpenScreenTags: default-size quiet screen, no title bar -----
        suba.l  a0,a0
        move.l  #0,-(sp)                  ; TAG_DONE data
        move.l  #TAG_DONE,-(sp)
        move.l  #0,-(sp)                  ; SA_ShowTitle = FALSE
        move.l  #SA_ShowTitle,-(sp)
        move.l  #1,-(sp)                  ; SA_Quiet = TRUE
        move.l  #SA_Quiet,-(sp)
        jsr     LVO_OpenScreenTags(a5)
        lea     24(sp),sp                 ; 3 tag pairs
        move.l  d0,d7                     ; d7 = screen
        beq.w   exit

        ; --- OpenWindowTags on the custom screen -------------------------
        suba.l  a0,a0
        move.l  #0,-(sp)                  ; TAG_DONE data
        move.l  #TAG_DONE,-(sp)
        move.l  #1,-(sp)                  ; WA_Activate
        move.l  #WA_Activate,-(sp)
        move.l  #IDCMP_CLOSEWINDOW,-(sp)  ; WA_IDCMP
        move.l  #WA_IDCMP,-(sp)
        move.l  #1,-(sp)                  ; WA_CloseGadget
        move.l  #WA_CloseGadget,-(sp)
        move.l  #1,-(sp)                  ; WA_DragBar
        move.l  #WA_DragBar,-(sp)
        move.l  #win_title,-(sp)          ; WA_Title
        move.l  #WA_Title,-(sp)
        move.l  #120,-(sp)                ; WA_Height
        move.l  #WA_Height,-(sp)
        move.l  #240,-(sp)                ; WA_Width
        move.l  #WA_Width,-(sp)
        move.l  #60,-(sp)                 ; WA_Top
        move.l  #WA_Top,-(sp)
        move.l  #60,-(sp)                 ; WA_Left
        move.l  #WA_Left,-(sp)
        move.l  d7,-(sp)                  ; WA_CustomScreen
        move.l  #WA_CustomScreen,-(sp)
        jsr     LVO_OpenWindowTags(a5)
        lea     88(sp),sp                 ; 11 tag pairs

        ; Deliberate leak: no CloseWindow, no CloseScreen, no CloseLibrary.
        ; The window/screen structures live in this task's RAM window —
        ; Task_Exit must retire the Intuition slots before releasing it.
exit:
        rts                               ; return address is the DOS Exit stub

        section data,data
libname_intuition: dc.b 'intuition.library',0
win_title:         dc.b 'LeakTest - orphan window',0
        even
