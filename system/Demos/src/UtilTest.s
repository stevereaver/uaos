; UtilTest.s — UAOS-238 acceptance: ROM module registry bound to guest
; OpenLibrary/OpenDevice.
;
; Covers:
;   * OpenLibrary("utility.library",37) returns a working base —
;     SMult32, UMult64, NextTagItem, GetTagData all dispatch.
;   * The version argument is honoured: utility.library v99 must fail.
;   * ToUpper/ToLower take and return a single character in D0 and
;     cover the international 0xC0-0xFE ranges.
;   * GetTagData follows TAG_MORE chains and honours TAG_IGNORE/TAG_SKIP.
;   * mathffp.library produces Motorola FFP bit patterns
;     (SPFlt(1)=$80000041) and SPCmp takes its operands in D1,D0 order.
;   * mathieeesingbas.library (generic ROM-bound base): IEEESPAdd works.
;   * dos.library StrToLong parses "  -42x" -> -42, 5 chars consumed.
;   * timer.device via OpenDevice: io_Device is a real base and the
;     AddTime vector (-66) dispatches to native code.
;   * Unknown libraries keep the predictable fake-base policy:
;     OpenLibrary("nonexistent.library") returns a base whose vectors
;     are no-ops returning 0.
;
; Prints "UTILTEST PASS" or "UTILTEST FAIL <stage>" on stdout.

LVO_OpenLibrary  equ -552
LVO_OpenDevice   equ -444
LVO_PutStr       equ -948
LVO_StrToLong    equ -816

; utility.library vectors
UL_GetTagData    equ -36
UL_NextTagItem   equ -48
UL_SMult32       equ -138
UL_UMult64       equ -204
UL_ToUpper       equ -174
UL_ToLower       equ -180

; mathffp.library vectors
MF_SPFlt         equ -36
MF_SPCmp         equ -42
MF_SPNeg         equ -60

; mathieeesingbas.library vectors
MB_IEEESPAdd     equ -66

; timer.device vectors
TD_AddTime       equ -66

IO_DEVICE        equ 20
IO_ERROR         equ 31

        section code,code

start:
        movea.l 4.w,a6
        move.l  a6,exec_base

        lea     dos_name(pc),a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,dos_base
        bne.s   .dos_ok
        rts
.dos_ok:
        lea     s_start(pc),a1
        bsr     print

; ---- version gate: utility.library v99 must fail ----------------------
        movea.l exec_base,a6
        lea     utl_name(pc),a1
        moveq   #99,d0
        jsr     LVO_OpenLibrary(a6)
        tst.l   d0
        beq.s   .vgate_ok
        lea     s_fvgate(pc),a1
        bra     do_fail
.vgate_ok:

; ---- utility.library v37 ----------------------------------------------
        lea     utl_name(pc),a1
        moveq   #37,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,utl_base
        bne.s   .utl_ok
        lea     s_futil(pc),a1
        bra     do_fail
.utl_ok:

        ; SMult32(6,7) == 42  (low 32 bits of the product)
        movea.l utl_base,a6
        moveq   #6,d0
        moveq   #7,d1
        jsr     UL_SMult32(a6)
        cmp.l   #42,d0
        beq.s   .sm_ok
        lea     s_fsmul(pc),a1
        bra     do_fail
.sm_ok:

        ; UMult64($FFFFFFFF,2) == $00000001:$FFFFFFFE
        move.l  #$FFFFFFFF,d0
        moveq   #2,d1
        jsr     UL_UMult64(a6)
        cmp.l   #1,d0
        bne.s   .um64_bad
        cmp.l   #$FFFFFFFE,d1
        beq.s   .um64_ok
.um64_bad:
        lea     s_fumul(pc),a1
        bra     do_fail
.um64_ok:

        ; NextTagItem(&tagptr) returns item 1, advances the pointer
        lea     tagptr(pc),a0
        jsr     UL_NextTagItem(a6)
        lea     taglist(pc),a1
        cmp.l   a1,d0
        bne.s   .nti_bad
        ; second call hits TAG_DONE -> NULL
        lea     tagptr(pc),a0
        jsr     UL_NextTagItem(a6)
        tst.l   d0
        beq.s   .nti_ok
.nti_bad:
        lea     s_fnti(pc),a1
        bra     do_fail
.nti_ok:

        ; GetTagData(5, taglist, 77) == $12345678
        moveq   #5,d0
        lea     taglist(pc),a0
        moveq   #77,d1
        jsr     UL_GetTagData(a6)
        cmp.l   #$12345678,d0
        beq.s   .gtd_ok
        lea     s_fgtd(pc),a1
        bra     do_fail
.gtd_ok:

        ; ToUpper('a') == 'A'; the international range folds 0xE0-0xFE
        ; except 0xF7, which stays unchanged.
        moveq   #'a',d0
        jsr     UL_ToUpper(a6)
        cmp.l   #'A',d0
        bne.s   .up_bad
        move.l  #$E0,d0
        jsr     UL_ToUpper(a6)
        cmp.l   #$C0,d0
        bne.s   .up_bad
        move.l  #$F7,d0
        jsr     UL_ToUpper(a6)
        cmp.l   #$F7,d0
        beq.s   .up_ok
.up_bad:
        lea     s_fupper(pc),a1
        bra     do_fail
.up_ok:

        ; ToLower('A') == 'a'; 0xC0-0xDE folds down, so $D7 -> $F7
        moveq   #'A',d0
        jsr     UL_ToLower(a6)
        cmp.l   #'a',d0
        bne.s   .low_bad
        move.l  #$D7,d0
        jsr     UL_ToLower(a6)
        cmp.l   #$F7,d0
        beq.s   .low_ok
.low_bad:
        lea     s_flower(pc),a1
        bra     do_fail
.low_ok:

        ; GetTagData follows a TAG_MORE chain; a tag hidden behind
        ; TAG_SKIP is not found and yields the default.
        moveq   #6,d0
        lea     chain1(pc),a0
        moveq   #0,d1
        jsr     UL_GetTagData(a6)
        cmp.l   #$00ABCDEF,d0
        bne.s   .chn_bad
        moveq   #7,d0
        lea     chain1(pc),a0
        moveq   #77,d1
        jsr     UL_GetTagData(a6)
        cmp.l   #77,d0
        beq.s   .chn_ok
.chn_bad:
        lea     s_fchain(pc),a1
        bra     do_fail
.chn_ok:

; ---- mathffp.library: Motorola FFP representation --------------------
        movea.l exec_base,a6
        lea     mffp_name(pc),a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,mffp_base
        bne.s   .ffp_ok
        lea     s_fmffp(pc),a1
        bra     do_fail
.ffp_ok:
        movea.l mffp_base,a6
        ; SPFlt(1) == $80000041 — FFP is mantissa<<8 | sign<<7 | exp+64
        moveq   #1,d0
        jsr     MF_SPFlt(a6)
        cmp.l   #$80000041,d0
        bne.s   .ffp_bad
        ; SPNeg(1.0) == $800000C1 — sign lives in low byte bit 7
        jsr     MF_SPNeg(a6)
        cmp.l   #$800000C1,d0
        bne.s   .ffp_bad
        ; SPCmp(D1=1.0, D0=2.0) == -1 — first operand is in D1
        move.l  #$80000041,d1
        move.l  #$80000042,d0
        jsr     MF_SPCmp(a6)
        cmp.l   #-1,d0
        beq.s   .ffp_done
.ffp_bad:
        lea     s_fffp(pc),a1
        bra     do_fail
.ffp_done:

; ---- mathieeesingbas.library: IEEESPAdd(2.0,3.0) == 5.0 ---------------
        movea.l exec_base,a6
        lea     misb_name(pc),a1
        moveq   #40,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,misb_base
        bne.s   .m_ok
        lea     s_fmisb(pc),a1
        bra     do_fail
.m_ok:
        movea.l misb_base,a6
        move.l  #$40000000,d0           ; 2.0f
        move.l  #$40400000,d1           ; 3.0f
        jsr     MB_IEEESPAdd(a6)
        cmp.l   #$40A00000,d0           ; 5.0f
        beq.s   .add_ok
        lea     s_fadd(pc),a1
        bra     do_fail
.add_ok:

; ---- timer.device via OpenDevice + AddTime vector ---------------------
        movea.l exec_base,a6
        lea     timer_name(pc),a0
        moveq   #0,d0                   ; unit
        lea     ioreq(pc),a1
        moveq   #0,d1                   ; flags
        jsr     LVO_OpenDevice(a6)
        tst.l   d0
        beq.s   .td_open
        lea     s_ftopen(pc),a1
        bra     do_fail
.td_open:
        move.l  ioreq+IO_DEVICE,d0
        bne.s   .td_base
        lea     s_ftbase(pc),a1
        bra     do_fail
.td_base:
        ; AddTime(dst={5,5}, src={1,1}) -> dst={6,6}
        movea.l d0,a6
        lea     tv_dst(pc),a0
        lea     tv_src(pc),a1
        jsr     TD_AddTime(a6)
        cmp.l   #6,tv_dst
        bne.s   .td_bad
        cmp.l   #6,tv_dst+4
        beq.s   .td_ok
.td_bad:
        lea     s_ftadd(pc),a1
        bra     do_fail
.td_ok:

; ---- dos.library: StrToLong("  -42x") -> 5 chars, value -42 -----------
        movea.l dos_base,a6
        lea     num_str(pc),a1
        move.l  a1,d1
        lea     longval(pc),a1
        move.l  a1,d2
        jsr     LVO_StrToLong(a6)
        cmp.l   #5,d0
        bne.s   .stl_bad
        cmp.l   #-42,longval
        beq.s   .stl_ok
.stl_bad:
        lea     s_fstol(pc),a1
        bra     do_fail
.stl_ok:

; ---- unknown library: fake base, vectors are no-op 0 ------------------
        movea.l exec_base,a6
        lea     bogus_name(pc),a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,bogus_base
        bne.s   .bg_ok
        lea     s_fbogus(pc),a1
        bra     do_fail
.bg_ok:
        movea.l bogus_base,a6
        jsr     -30(a6)
        tst.l   d0
        beq.s   .bg_noop
        lea     s_fnoop(pc),a1
        bra     do_fail
.bg_noop:

        lea     s_pass(pc),a1
        bsr     print
        moveq   #0,d0
        rts

do_fail:
        bsr     print
        moveq   #10,d0                  ; RETURN_WARN
        rts

print:                                  ; a1 = NUL-terminated string
        movem.l d1/a6,-(sp)
        move.l  a1,d1
        movea.l dos_base,a6
        jsr     LVO_PutStr(a6)
        movem.l (sp)+,d1/a6
        rts

; ---- data ------------------------------------------------------------

        even
exec_base:  dc.l 0
dos_base:   dc.l 0
utl_base:   dc.l 0
mffp_base:  dc.l 0
misb_base:  dc.l 0
bogus_base: dc.l 0
tagptr:     dc.l taglist
taglist:    dc.l 5,$12345678            ; TagItem { ti_Tag=5, ti_Data }
            dc.l 0,0                    ; TAG_DONE
chain1:     dc.l 2,chain2               ; TAG_MORE -> chain2
            dc.l 0,0                    ; TAG_DONE
chain2:     dc.l 3,1                    ; TAG_SKIP: skip this + 1 more
            dc.l 7,$DEADBEEF            ; skipped
            dc.l 6,$00ABCDEF
            dc.l 0,0                    ; TAG_DONE
longval:    dc.l 0
tv_dst:     dc.l 5,5
tv_src:     dc.l 1,1
ioreq:      ds.b 48

dos_name:   dc.b "dos.library",0
utl_name:   dc.b "utility.library",0
mffp_name:  dc.b "mathffp.library",0
misb_name:  dc.b "mathieeesingbas.library",0
num_str:    dc.b "  -42x",0
timer_name: dc.b "timer.device",0
bogus_name: dc.b "nonexistent.library",0

s_start:    dc.b "UTILTEST begin",10,0
s_pass:     dc.b "UTILTEST PASS",10,0

s_fvgate:   dc.b "UTILTEST FAIL version gate (v99 accepted)",10,0
s_futil:    dc.b "UTILTEST FAIL OpenLibrary(utility)",10,0
s_fsmul:    dc.b "UTILTEST FAIL SMult32",10,0
s_fumul:    dc.b "UTILTEST FAIL UMult64",10,0
s_fnti:     dc.b "UTILTEST FAIL NextTagItem",10,0
s_fgtd:     dc.b "UTILTEST FAIL GetTagData",10,0
s_fupper:   dc.b "UTILTEST FAIL ToUpper",10,0
s_flower:   dc.b "UTILTEST FAIL ToLower",10,0
s_fchain:   dc.b "UTILTEST FAIL tag chain/skip",10,0
s_fmffp:    dc.b "UTILTEST FAIL OpenLibrary(mathffp)",10,0
s_fffp:     dc.b "UTILTEST FAIL mathffp FFP ops",10,0
s_fstol:    dc.b "UTILTEST FAIL StrToLong",10,0
s_fmisb:    dc.b "UTILTEST FAIL OpenLibrary(mathieeesingbas)",10,0
s_fadd:     dc.b "UTILTEST FAIL IEEESPAdd",10,0
s_ftopen:   dc.b "UTILTEST FAIL OpenDevice(timer)",10,0
s_ftbase:   dc.b "UTILTEST FAIL timer io_Device NULL",10,0
s_ftadd:    dc.b "UTILTEST FAIL AddTime",10,0
s_fbogus:   dc.b "UTILTEST FAIL OpenLibrary(nonexistent) NULL",10,0
s_fnoop:    dc.b "UTILTEST FAIL fake vector returned nonzero",10,0

        end
