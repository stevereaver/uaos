; BeepTest.s — UAOS-19: guest DisplayBeep drives the audible error beep.
;
; Opens intuition.library, calls DisplayBeep(NULL) — the canonical AmigaOS
; error-feedback call — then prints BEEPTEST DONE on stdout/serial.  The
; flash runs ~50 ms; the queued square-wave beep (~120 ms) is verified
; host-side by capturing the PC-speaker output to a WAV in QEMU.

LVO_OpenLibrary  equ -552
LVO_CloseLibrary equ -414
LVO_PutStr       equ -948
LVO_DisplayBeep  equ -96
LV_RawPutChar    equ -516

        section code,code

start:
        movea.l 4.w,a6
        move.l  a6,exec_base

        lea     dos_name(pc),a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,dos_base
        beq     .early_exit

        lea     int_name(pc),a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,int_base
        beq     .fail_int

        movea.l int_base,a6
        suba.l  a0,a0                   ; screen = NULL (frontmost)
        jsr     LVO_DisplayBeep(a6)

        ; close intuition.library
        movea.l exec_base,a6
        movea.l int_base,a1
        jsr     LVO_CloseLibrary(a6)

        lea     s_done(pc),a1
        bsr     print
        bra.s   .out

.fail_int:
        lea     s_fint(pc),a1
        bsr     print
.out:
        movea.l exec_base,a6
        movea.l dos_base,a1
        jsr     LVO_CloseLibrary(a6)
.early_exit:
        moveq   #0,d0
        rts

print:                                  ; a1 = NUL-terminated string
        movem.l d0-d2/a0-a2/a6,-(sp)
        move.l  a1,d1
        movea.l dos_base,a6
        jsr     LVO_PutStr(a6)
        ; echo to serial via exec/RawPutChar so headless tests can read it
        move.l  a1,a2
        movea.l exec_base,a6
.rp_loop:
        move.b  (a2)+,d0
        beq.s   .rp_done
        jsr     LV_RawPutChar(a6)
        bra.s   .rp_loop
.rp_done:
        movem.l (sp)+,d0-d2/a0-a2/a6
        rts

exec_base:  dc.l 0
dos_base:   dc.l 0
int_base:   dc.l 0

dos_name:   dc.b "dos.library",0
int_name:   dc.b "intuition.library",0
s_done:     dc.b "BEEPTEST DONE",10,0
s_fint:     dc.b "BEEPTEST FAIL intuition.library",10,0
        even
