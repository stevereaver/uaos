; AudIrqTest.s — UAOS-244 runtime probe for the Paula direct-register path.
;
; A guest pokes AUDx/DMACON/INTENA directly (no audio.device), exactly like
; OctaMED-class trackers do:
;   * AllocMem(MEMF_CHIP) buffer must land inside the Paula DMA window and
;     AUDxLCH/LCL writes must bind that window for the sampler.
;   * Each channel gets a SetIntVector'd level-4 server (INTREQ bits 7-10).
;     The ISR counts block-complete IRQs, clears its INTREQ bit and
;     re-arms AUDxLC/LEN — the classic IRQ-driven player loop.
;   * With DMACON AUDEN+AUD0-3 set and ~2 s of dos.Delay, every channel
;     must rack up block-complete interrupts; then a ~4-tone chord plays
;     for 3 s so the result is audible on real hardware.
;
;   AUDIRQ PASS  -> all 4 channels delivered block-complete IRQs at L4
;   AUDIRQ FAIL  -> prints which stage/chan failed
;
; Assemble: vasmm68k_mot -Fhunk -o AudIrqTest.o AudIrqTest.s
; Link:     vlink -bamigahunk -o AudIrqTest.hunk AudIrqTest.o
;

CUSTOM       equ $00DFF000
AUD0_BASE    equ $0A0              ; AUDx base: +0 LCH +2 LCL +4 LEN +6 PER +8 VOL
DMACON       equ $096
DMACONR      equ $002
INTENA       equ $09A
INTENAR      equ $01C
INTREQ       equ $09C
INTREQR      equ $01E

LVO_OpenLibrary  equ -552
LVO_CloseLibrary equ -414
LVO_PutStr       equ -948
LVO_Delay        equ -198

LV_AllocMem      equ -198
LV_FreeMem       equ -210
LV_SetIntVector  equ -162
LV_RawPutChar    equ -516

MEMF_CHIP    equ $00000002
MEMF_CLEAR   equ $00010000

BUF_SIZE     equ 2048
IRQ_LEN      equ 100               ; words per DMA block during count phase
IRQ_PER_BASE equ 200               ; AUD0PER=200, +40 per channel

; is_Data record layout (per channel):
;   +0 counter.l  +4 regoff.w  +6 intbit.w  +8 len.w  +10 buf.l
REC_SIZE     equ 16

        section code,code

start:
        movea.l 4.w,a6
        move.l  a6,exec_base

        lea     dos_name(pc),a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,dos_base
        beq     early_exit
        lea     s_start(pc),a1
        bsr     print

; ---- chip RAM buffer --------------------------------------------------
        movea.l exec_base,a6
        move.l  #BUF_SIZE,d0
        move.l  #(MEMF_CHIP|MEMF_CLEAR),d1
        jsr     LV_AllocMem(a6)
        move.l  d0,buf
        bne.s   .buf_ok
        lea     s_fmem(pc),a1
        bra     do_fail
.buf_ok:
        ; must live inside the 8 MB Paula DMA window
        cmp.l   #$00800000,d0
        blo.s   .in_chip
        lea     s_frange(pc),a1
        bra     do_fail
.in_chip:
        lea     s_bufat(pc),a1
        bsr     print
        move.l  buf,d0
        bsr     printhex
        lea     s_nl(pc),a1
        bsr     print

        ; fill: alternating runs of 8 x $50 / 8 x $B0 (bipolar square)
        movea.l buf,a0
        move.w  #BUF_SIZE/8-1,d1
        moveq   #0,d2
.fill:
        moveq   #$50,d0
        btst    #3,d2
        beq.s   .hi
        move.b  #$B0,d0
.hi:
        move.b  d0,(a0)+
        addq.w  #1,d2
        dbra    d1,.fill

; ---- per-channel records + interrupt nodes ----------------------------
        moveq   #3,d7
.setup:
        move.w  d7,d0
        mulu    #REC_SIZE,d0
        lea     recs,a3
        adda.w  d0,a3                   ; recs[ch]
        lea     intrs,a4
        move.w  d7,d0
        mulu    #22,d0
        adda.w  d0,a4                   ; intrs[ch]
        move.w  d7,d0
        lsl.w   #4,d0                   ; ch*0x10
        add.w   #AUD0_BASE,d0
        move.w  d0,4(a3)                ; regoff
        move.w  #$0080,d1
        lsl.w   d7,d1                   ; INTREQ bit 7+ch
        move.w  d1,6(a3)                ; intbit
        move.w  #IRQ_LEN,8(a3)
        move.l  buf,10(a3)
        ; Interrupt node: ln_Type=NT_INTERRUPT(2), name, is_Data, is_Code
        move.b  #2,8(a4)
        lea     isr_name(pc),a0
        move.l  a0,10(a4)
        move.l  a3,14(a4)               ; is_Data = record
        lea     aud_isr(pc),a0
        move.l  a0,18(a4)               ; is_Code
        ; SetIntVector(7+ch, intr)
        moveq   #7,d0
        add.w   d7,d0
        movea.l a4,a1
        movea.l exec_base,a6
        jsr     LV_SetIntVector(a6)
        dbra    d7,.setup

; ---- program AUD0-3 ---------------------------------------------------
        movea.l #CUSTOM,a5
        move.l  buf,d1
        moveq   #3,d7
.prog:
        move.w  d7,d0
        lsl.w   #4,d0
        movea.l a5,a2
        adda.w  #AUD0_BASE,a2
        adda.w  d0,a2                   ; a2 = $DFF000 + $0A0 + ch*$10
        move.l  d1,d2
        swap    d2
        move.w  d2,(a2)                 ; AUDxLCH
        move.w  d1,2(a2)                ; AUDxLCL
        move.w  #IRQ_LEN,4(a2)          ; AUDxLEN
        move.w  #IRQ_PER_BASE,d2
        move.w  d7,d3
        mulu    #40,d3
        add.w   d3,d2
        move.w  d2,6(a2)                ; AUDxPER
        move.w  #48,8(a2)               ; AUDxVOL
        dbra    d7,.prog

; ---- enable: INTENA master+AUD0-3, then DMACON DMAEN+AUD0-3 -----------
        move.w  #$C780,INTENA(a5)
        move.w  #$820F,DMACON(a5)

        lea     s_counting(pc),a1
        bsr     print
        movea.l dos_base,a6
        move.l  #100,d1                 ; ~2 s; wait path pumps IRQ delivery
        move.l  d1,d0                   ; legacy UAOS build read d0 — belt+braces
        jsr     LVO_Delay(a6)

; ---- report per-channel IRQ counts ------------------------------------
        moveq   #0,d7                   ; ch index for display
        lea     recs,a3
        moveq   #1,d6                   ; all-ok flag
.rep:
        lea     s_ch(pc),a1
        bsr     print
        move.l  d7,d0
        add.l   #'0',d0
        move.b  d0,ch_digit
        lea     ch_digit(pc),a1
        bsr     print
        lea     s_eq(pc),a1
        bsr     print
        move.l  (a3),d0
        move.l  d0,d5                   ; keep for check
        bsr     printhex
        lea     s_nl(pc),a1
        bsr     print
        cmp.l   #4,d5                   ; expect ~40 in 2 s; >=4 proves the chain
        bge.s   .ch_ok
        moveq   #0,d6
.ch_ok:
        adda.l  #REC_SIZE,a3
        addq.w  #1,d7
        cmp.w   #4,d7
        blo.s   .rep

        tst.l   d6
        bne.s   .irqs_ok
        lea     s_firq(pc),a1
        bra     do_fail
.irqs_ok:
        lea     s_pass1(pc),a1
        bsr     print

; ---- leave the chord playing ~3 s so it's audible on hardware --------
        lea     s_tone(pc),a1
        bsr     print
        movea.l dos_base,a6
        move.l  #150,d1
        move.l  d1,d0                   ; legacy UAOS build read d0 — belt+braces
        jsr     LVO_Delay(a6)

; ---- shut it down ------------------------------------------------------
        movea.l #CUSTOM,a5
        move.w  #$000F,DMACON(a5)       ; clear AUD0-3EN
        move.w  #$0200,DMACON(a5)       ; clear DMAEN master
        move.w  #$4780,INTENA(a5)       ; clear INTEN + AUD bits
        movea.l exec_base,a6
        move.l  buf,a1
        move.l  #BUF_SIZE,d0
        jsr     LV_FreeMem(a6)

        lea     s_pass(pc),a1
        bsr     print
        bra     out

do_fail:
        bsr     print
        movea.l #CUSTOM,a5
        move.w  #$020F,DMACON(a5)       ; belt+braces: DMA off on fail
        move.w  #$4780,INTENA(a5)
        lea     s_fail(pc),a1
        bsr     print
out:
        movea.l exec_base,a6
        movea.l dos_base,a1
        jsr     LVO_CloseLibrary(a6)
early_exit:
        rts

; -----------------------------------------------------------------------
; aud_isr — level-4 server, entered per Amiga convention:
;   D0 = pending INTREQ bits, A0 = $DFF000, A1 = is_Data record.
; Counts the IRQ, clears its INTREQ bit, re-arms LC+LEN (player loop).
; -----------------------------------------------------------------------
aud_isr:
        addq.l  #1,(a1)                 ; counter++
        move.w  6(a1),d0
        move.w  d0,INTREQ(a0)           ; clear our INTREQ bit
        move.w  4(a1),d0
        lea     0(a0,d0.w),a2           ; a2 = &AUDx regs
        move.w  8(a1),4(a2)             ; AUDxLEN re-arm
        move.l  10(a1),d0
        move.w  d0,2(a2)                ; AUDxLCL
        swap    d0
        move.w  d0,(a2)                 ; AUDxLCH (rewinds ptr, rebinds window)
        moveq   #0,d0                   ; keep chain going
        rts

print:                                  ; a1 = NUL-terminated string
        movem.l d0-d2/a0-a2/a6,-(sp)
        move.l  a1,d1
        movea.l dos_base,a6
        jsr     LVO_PutStr(a6)
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

printhex:                               ; d0 = value, prints 8 hex digits
        movem.l d0-d3/a0-a2/a6,-(sp)
        lea     hexbuf,a0
        moveq   #7,d3
.hx:
        rol.l   #4,d0
        move.l  d0,d2
        and.l   #$F,d2
        add.b   #'0',d2
        cmp.b   #'9',d2
        ble.s   .d
        add.b   #7,d2
.d:     move.b  d2,(a0)+
        dbra    d3,.hx
        clr.b   (a0)
        lea     hexbuf,a1
        bsr     print
        movem.l (sp)+,d0-d3/a0-a2/a6
        rts

        ; single-section layout: PC-relative refs resolve at assembly time
        section code,code

dos_name:   dc.b "dos.library",0
        even
isr_name:   dc.b "audirq",0
        even
s_start:    dc.b "AUDIRQTEST (UAOS-244)",10,0
        even
s_bufat:    dc.b "chip buffer at 0x",0
        even
s_counting: dc.b "counting AUD0-3 block-complete IRQs for ~2s",10,0
        even
s_ch:       dc.b "  AUD",0
        even
ch_digit:   dc.b "0",0
        even
s_eq:       dc.b " irq count=0x",0
        even
s_nl:       dc.b 10,0
        even
s_tone:     dc.b "playing ~3s chord on AUD0-3 (listen)",10,0
        even
s_fmem:     dc.b "FAIL AllocMem chip",10,0
        even
s_frange:   dc.b "FAIL buffer outside Paula DMA window",10,0
        even
s_firq:     dc.b "FAIL missing block-complete IRQs",10,0
        even
s_pass1:    dc.b "all 4 channels delivered level-4 block-complete IRQs",10,0
        even
s_pass:     dc.b "AUDIRQ PASS",10,0
        even
s_fail:     dc.b "AUDIRQ FAIL",10,0
        even

exec_base:  ds.l 1
dos_base:   ds.l 1
buf:        ds.l 1
hexbuf:     ds.b 12
        even
recs:       ds.b REC_SIZE*4
intrs:      ds.b 22*4

        end
