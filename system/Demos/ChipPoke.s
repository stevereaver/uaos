; ChipPoke.s — poke a handful of custom-chip/CIA registers, then exit.
;
; Companion test for C:chiptrace: every access below should show up in
; klog as a decoded line, e.g. "W DFF180 COLOR00 = 0xf0f  pc=0003xx".
; Run with:  chiptrace on ; Demos/ChipPoke ; chiptrace off
;
; Assemble: vasmm68k_mot -Fhunk -o ChipPoke.o ChipPoke.s
; Link:     vlink -bamigahunk -o ChipPoke.hunk ChipPoke.o
;

DOS_BASE        equ $00000800
LVO_DOS_Exit    equ -144

CUSTOM          equ $00DFF000
CIA_A_PRA       equ $00BFE001        ; CIA-A PRA (odd byte lane)
CIA_B_DDRA      equ $00BFD200        ; CIA-B DDRA (reg 2 * $100)

        section code,code

start:
        lea     CUSTOM,a5

        ; COLOR00 — palette write
        move.w  #$0F0F,$180(a5)

        ; DENISEID x8 — constant read, exercises chiptrace repeat folding
        moveq   #7,d1
.rep:   move.w  $07C(a5),d0
        dbra    d1,.rep

        ; beam position + DMA status reads
        move.w  $006(a5),d0           ; VHPOSR
        move.w  $002(a5),d0           ; DMACONR

        ; read-modify-write INTENA/INTREQ with unchanged values (no-op)
        move.w  $01C(a5),d2           ; INTENAR
        move.w  d2,$09A(a5)           ; INTENA (same bits, no change)
        move.w  $01E(a5),d2           ; INTREQR
        move.w  d2,$09C(a5)           ; INTREQ (same bits, no change)

        ; Paula audio regs — AUD0VOL=0 (mute), AUD0PER=68
        clr.w   $0A8(a5)
        move.w  #68,$0A6(a5)

        ; floppy regs — DSKSYNC standard sync (no DMA triggered)
        move.w  #$4489,$07E(a5)

        ; CIA registers
        move.b  #$FF,CIA_A_PRA
        move.b  #$55,CIA_B_DDRA

        ; exit back to DOS
        movea.l #DOS_BASE,a6
        moveq   #0,d0
        jsr     LVO_DOS_Exit(a6)
.exit:  bra     .exit

        end
