; CIATest.s — UAOS-241 acceptance test for guest interrupt delivery.
;
;   1. exec Cause() software interrupt            -> co_count
;   2. SetIntVector(13) exec EXTER server         -> ex_count
;   3. ciab.resource AddICRVector(bit 0) + CIA-B
;      Timer A programmed at ~100 Hz              -> ta_count
;
; Phase A (~2 s of Delay): the handlers must fire while the task
; sleeps/wakes.  Phase B masks ICR bit 0 via AbleICR: the timer
; handler (and the EXTER line it feeds) must go silent.
;
; Prints "CIATEST PASS/FAIL" plus hex counters on stdout.

LVO_OpenResource  equ -498
LVO_SetIntVector  equ -162
LVO_Cause         equ -180
LVO_OpenLibrary   equ -552
LVO_Delay         equ -198
LVO_PutStr        equ -948

INTENA      equ $00DFF09A
CIAB_TALO   equ $00BFD400
CIAB_TAHI   equ $00BFD500
CIAB_CRA    equ $00BFDE00
TA_LATCH    equ 7094            ; ~100 Hz at the 709379 Hz E-clock

        section code,code

start:
        movea.l 4.w,a6
        move.l  a6,exec_base

        ; dos.library for PutStr
        lea     dos_name(pc),a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,dos_base
        bne.s   .dos_ok
        rts
.dos_ok:
        lea     s_start(pc),a1
        bsr     print

        ; ciab.resource
        movea.l exec_base,a6
        lea     ciab_name(pc),a1
        jsr     LVO_OpenResource(a6)
        move.l  d0,ciab_base
        bne.s   .res_ok
        lea     s_fres(pc),a1
        bra     do_fail
.res_ok:

        ; Cause() software interrupt — fires immediately
        movea.l exec_base,a6
        lea     co_intr(pc),a1
        jsr     LVO_Cause(a6)
        tst.l   co_count
        bne.s   .cause_ok
        lea     s_fcause(pc),a1
        bra     do_fail
.cause_ok:

        ; SetIntVector(13, ex_intr) — EXTER exec server (direct path)
        movea.l exec_base,a6
        moveq   #13,d0
        lea     ex_intr(pc),a1
        jsr     LVO_SetIntVector(a6)

        ; AddICRVector(bit 0 = Timer A, ta_intr)
        movea.l ciab_base,a6
        moveq   #0,d0
        lea     ta_intr(pc),a1
        jsr     -6(a6)
        tst.l   d0
        beq.s   .icr_ok
        lea     s_ficr(pc),a1
        bra     do_fail
.icr_ok:

        ; Enable EXTER (bit 13) via the master-set form of INTENA
        move.w  #$A000,INTENA

        ; CIA-B Timer A: latch 7094 (~100 Hz), continuous run
        move.b  #$00,CIAB_CRA
        move.b  #(TA_LATCH&$FF),CIAB_TALO
        move.b  #(TA_LATCH>>8),CIAB_TAHI
        move.b  #$01,CIAB_CRA

        ; Phase A: 50 x Delay(2) ~= 2 s wall — handlers tick between wakes
        moveq   #49,d7
.wa:    movea.l dos_base,a6
        moveq   #2,d1
        jsr     LVO_Delay(a6)
        dbra    d7,.wa

        move.l  ta_count,d6         ; TA deliveries so far
        move.l  ex_count,d5         ; EXTER server deliveries

        ; Phase B: AbleICR(0x01) clears the mask -> deliveries must stop
        movea.l ciab_base,a6
        moveq   #1,d0
        jsr     -18(a6)
        moveq   #19,d7
.wb:    movea.l dos_base,a6
        moveq   #2,d1
        jsr     LVO_Delay(a6)
        dbra    d7,.wb
        move.l  ta_count,d4
        sub.l   d6,d4               ; masked-phase TA delta (want 0)
        move.l  ex_count,d3
        sub.l   d5,d3               ; masked-phase EXTER delta (want 0)

        ; verdict
        tst.l   d6
        beq     f_ta
        tst.l   d5
        beq     f_ex
        tst.l   d4
        bne     f_mask
        tst.l   d3
        bne     f_mask

        ; PASS — print counters: ta=, ex=, co=, masked delta dm=
        lea     s_pass(pc),a1
        bsr     print
        lea     s_ta(pc),a1
        move.l  d6,d0
        bsr     print_num
        lea     s_ex(pc),a1
        move.l  d5,d0
        bsr     print_num
        lea     s_co(pc),a1
        move.l  co_count,d0
        bsr     print_num
        lea     s_crlf(pc),a1
        bsr     print
        rts

f_ta:   lea     s_fta(pc),a1
        bra     do_fail
f_ex:   lea     s_fex(pc),a1
        bra     do_fail
f_mask: lea     s_fmask(pc),a1
        bra     do_fail

do_fail:                           ; a1 = message
        bsr     print
        lea     s_crlf(pc),a1
        bsr     print
        moveq   #5,d7           ; brief settle then exit
.fx:    movea.l dos_base,a6
        moveq   #2,d1
        jsr     LVO_Delay(a6)
        dbra    d7,.fx
        rts

; ---- helpers ---------------------------------------------------------

print:                          ; a1 = NUL-terminated string
        movem.l d1/a6,-(sp)
        move.l  a1,d1
        movea.l dos_base,a6
        jsr     LVO_PutStr(a6)
        movem.l (sp)+,d1/a6
        rts

print_num:                      ; a1 = label, d0 = value -> label + hex8 + ' '
        movem.l d0-d1/a0-a1,-(sp)
        bsr     print
        lea     numbuf,a0
        move.l  (sp),d0         ; saved d0 (first reg in the movem mask)
        moveq   #7,d1
.hx:    rol.l   #4,d0
        move.b  d0,d2
        and.b   #$0F,d2
        cmp.b   #10,d2
        blt.s   .dig
        add.b   #'A'-10-'0',d2
.dig:   add.b   #'0',d2
        move.b  d2,(a0)+
        dbra    d1,.hx
        move.b  #' ',(a0)+
        move.b  #0,(a0)
        lea     numbuf,a1
        bsr     print
        movem.l (sp)+,d0-d1/a0-a1
        rts

; ---- interrupt handlers ----------------------------------------------

co_handler:                     ; Cause(): A1 = is_Data
        addq.l  #1,(a1)
        rts

ex_handler:                     ; IntVects EXTER: D0=INTREQ bit, A1=is_Data
        addq.l  #1,(a1)
        moveq   #1,d0            ; claim it (chain-dispatch semantics)
        rts

ta_handler:                     ; ICR: A0=CIA-B base, A1=is_Data, D0=bit
        addq.l  #1,(a1)
        rts

; ---- data ------------------------------------------------------------

        even
exec_base:  dc.l 0
dos_base:   dc.l 0
ciab_base:  dc.l 0
co_count:   dc.l 0
ex_count:   dc.l 0
ta_count:   dc.l 0

; struct Interrupt: ln_Succ,ln_Pred,ln_Type.B,ln_Pri.B,ln_Name,is_Data,is_Code
co_intr:    dc.l 0,0
            dc.b 9,0
            dc.l s_con
            dc.l co_count
            dc.l co_handler
ex_intr:    dc.l 0,0
            dc.b 9,0
            dc.l s_exn
            dc.l ex_count
            dc.l ex_handler
ta_intr:    dc.l 0,0
            dc.b 9,0
            dc.l s_tan
            dc.l ta_count
            dc.l ta_handler

dos_name:   dc.b "dos.library",0
ciab_name:  dc.b "ciab.resource",0
s_start:    dc.b "CIATEST begin",10,0
s_con:      dc.b "cause",0
s_exn:      dc.b "exter",0
s_tan:      dc.b "cita",0
s_pass:     dc.b "CIATEST PASS ",0
s_ta:       dc.b "ta=",0
s_ex:       dc.b "ex=",0
s_co:       dc.b "co=",0
s_crlf:     dc.b 10,0
s_fres:     dc.b "CIATEST FAIL: ciab.resource",10,0
s_fcause:   dc.b "CIATEST FAIL: cause",10,0
s_ficr:     dc.b "CIATEST FAIL: AddICRVector",10,0
s_fta:      dc.b "CIATEST FAIL: timer ticks=0",10,0
s_fex:      dc.b "CIATEST FAIL: exter ticks=0",10,0
s_fmask:    dc.b "CIATEST FAIL: mask",10,0

numbuf:     ds.b 16

        end
