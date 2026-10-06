; IFFTest.s — UAOS-243 iffparse.library acceptance test.
;
; Phase 1: write a small FORM 8SVX (VHDR + BODY) through iffparse WRITE
;          mode onto RAM:ifftest.8svx.
; Phase 2: read it back — PropChunk(VHDR), StopChunk(BODY), SCAN parse,
;          FindProp, CurrentChunk, ReadChunkBytes.
; Phase 3: reopen and walk the file with IFFPARSE_RAWSTEP, checking the
;          enter/leave/EOF return sequence.
;
; Prints "IFFTEST PASS" or "IFFTEST FAIL <stage>" on stdout.

LVO_OpenLibrary  equ -552
LVO_CloseLibrary equ -414
LVO_Open         equ -30
LVO_Close        equ -36
LVO_PutStr       equ -948
MODE_OLDFILE     equ 1005
MODE_NEWFILE     equ 1006

IL_AllocIFF        equ -30
IL_OpenIFF         equ -36
IL_ParseIFF        equ -42
IL_CloseIFF        equ -48
IL_FreeIFF         equ -54
IL_ReadChunkBytes  equ -60
IL_WriteChunkBytes equ -66
IL_PushChunk       equ -84
IL_PopChunk        equ -90
IL_PropChunk       equ -114
IL_StopChunk       equ -126
IL_FindProp        equ -156
IL_CurrentChunk    equ -174
IL_InitIFFasDOS    equ -234

ID_FORM            equ $464F524D
T_8SVX             equ $38535658
ID_VHDR            equ $56484452
ID_BODY            equ $424F4459

IFFSIZE_UNKNOWN    equ -1
IFFF_READ          equ 0
IFFF_WRITE         equ 1
IFFPARSE_SCAN      equ 0
IFFPARSE_RAWSTEP   equ 2

IFFERR_EOF         equ -2
IFFERR_EOC         equ -3

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

        movea.l exec_base,a6
        lea     iff_name(pc),a1
        moveq   #37,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,iff_base
        bne.s   .iff_ok
        lea     s_flib(pc),a1
        bra     do_fail
.iff_ok:

; ======================== write phase =================================
        lea     s_wr(pc),a1
        bsr     print

        movea.l dos_base,a6
        lea     file_name(pc),a0
        move.l  a0,d1
        move.l  #MODE_NEWFILE,d2
        jsr     LVO_Open(a6)
        move.l  d0,d7
        bne.s   .w_open
        lea     s_fwopen(pc),a1
        bra     do_fail
.w_open:
        movea.l iff_base,a6
        jsr     IL_AllocIFF(a6)
        move.l  d0,a2
        bne.s   .w_iff
        lea     s_falloc(pc),a1
        bra     do_fail
.w_iff:
        move.l  d7,(a2)                 ; iff->iff_Stream = filehandle
        move.l  a2,a0
        jsr     IL_InitIFFasDOS(a6)
        move.l  a2,a0
        moveq   #IFFF_WRITE,d0
        jsr     IL_OpenIFF(a6)
        tst.l   d0
        beq.s   .w_opened
        lea     s_fwiff(pc),a1
        bra     do_fail
.w_opened:
        ; PushChunk(iff, '8SVX', 'FORM', IFFSIZE_UNKNOWN)
        move.l  a2,a0
        move.l  #T_8SVX,d0
        move.l  #ID_FORM,d1
        move.l  #IFFSIZE_UNKNOWN,d2
        jsr     IL_PushChunk(a6)
        tst.l   d0
        beq.s   .w_form
        lea     s_fpush(pc),a1
        bra     do_fail
.w_form:
        ; PushChunk(iff, 0, 'VHDR', 20)
        move.l  a2,a0
        moveq   #0,d0
        move.l  #ID_VHDR,d1
        moveq   #20,d2
        jsr     IL_PushChunk(a6)
        tst.l   d0
        beq.s   .w_vhdr
        lea     s_fpush(pc),a1
        bra     do_fail
.w_vhdr:
        move.l  a2,a0
        lea     vhdr_data(pc),a1
        moveq   #20,d0
        jsr     IL_WriteChunkBytes(a6)
        cmp.l   #20,d0
        beq.s   .w_vdata
        lea     s_fwrch(pc),a1
        bra     do_fail
.w_vdata:
        move.l  a2,a0
        jsr     IL_PopChunk(a6)
        tst.l   d0
        beq.s   .w_vpop
        lea     s_fpop(pc),a1
        bra     do_fail
.w_vpop:
        ; PushChunk(iff, 0, 'BODY', 8)
        move.l  a2,a0
        moveq   #0,d0
        move.l  #ID_BODY,d1
        moveq   #8,d2
        jsr     IL_PushChunk(a6)
        tst.l   d0
        beq.s   .w_body
        lea     s_fpush(pc),a1
        bra     do_fail
.w_body:
        move.l  a2,a0
        lea     body_data(pc),a1
        moveq   #8,d0
        jsr     IL_WriteChunkBytes(a6)
        cmp.l   #8,d0
        beq.s   .w_bdata
        lea     s_fwrch(pc),a1
        bra     do_fail
.w_bdata:
        move.l  a2,a0
        jsr     IL_PopChunk(a6)
        tst.l   d0
        beq.s   .w_bpop
        lea     s_fpop(pc),a1
        bra     do_fail
.w_bpop:
        move.l  a2,a0
        jsr     IL_PopChunk(a6)         ; pops the FORM, patches size
        tst.l   d0
        beq.s   .w_fpop
        lea     s_fpop(pc),a1
        bra     do_fail
.w_fpop:
        move.l  a2,a0
        jsr     IL_CloseIFF(a6)
        move.l  a2,a0
        jsr     IL_FreeIFF(a6)
        movea.l dos_base,a6
        move.l  d7,d1
        jsr     LVO_Close(a6)

; ======================== read phase ==================================
        lea     s_rd(pc),a1
        bsr     print

        movea.l dos_base,a6
        lea     file_name(pc),a0
        move.l  a0,d1
        move.l  #MODE_OLDFILE,d2
        jsr     LVO_Open(a6)
        move.l  d0,d7
        bne.s   .r_open
        lea     s_fropen(pc),a1
        bra     do_fail
.r_open:
        movea.l iff_base,a6
        jsr     IL_AllocIFF(a6)
        move.l  d0,a2
        bne.s   .r_iff
        lea     s_falloc(pc),a1
        bra     do_fail
.r_iff:
        move.l  d7,(a2)
        move.l  a2,a0
        jsr     IL_InitIFFasDOS(a6)

        ; PropChunk(iff, '8SVX', 'VHDR')
        move.l  a2,a0
        move.l  #T_8SVX,d0
        move.l  #ID_VHDR,d1
        jsr     IL_PropChunk(a6)
        tst.l   d0
        beq.s   .r_prop
        lea     s_fprop(pc),a1
        bra     do_fail
.r_prop:
        ; StopChunk(iff, '8SVX', 'BODY')
        move.l  a2,a0
        move.l  #T_8SVX,d0
        move.l  #ID_BODY,d1
        jsr     IL_StopChunk(a6)
        tst.l   d0
        beq.s   .r_stop
        lea     s_fstopc(pc),a1
        bra     do_fail
.r_stop:
        move.l  a2,a0
        moveq   #IFFF_READ,d0
        jsr     IL_OpenIFF(a6)
        tst.l   d0
        beq.s   .r_opened
        lea     s_friff(pc),a1
        bra     do_fail
.r_opened:
        move.l  a2,a0
        moveq   #IFFPARSE_SCAN,d0
        jsr     IL_ParseIFF(a6)
        tst.l   d0
        beq.s   .r_scanned
        lea     s_fparse(pc),a1
        bra     do_fail
.r_scanned:
        ; CurrentChunk must be BODY inside an 8SVX form
        move.l  a2,a0
        jsr     IL_CurrentChunk(a6)
        tst.l   d0
        bne.s   .r_cc
        lea     s_fcc(pc),a1
        bra     do_fail
.r_cc:
        move.l  d0,a0
        cmp.l   #ID_BODY,8(a0)          ; cn_ID
        beq.s   .r_ccid
        lea     s_fccid(pc),a1
        bra     do_fail
.r_ccid:
        cmp.l   #T_8SVX,12(a0)          ; cn_Type
        beq.s   .r_cct
        lea     s_fcct(pc),a1
        bra     do_fail
.r_cct:
        ; FindProp(iff, '8SVX', 'VHDR') -> StoredProperty
        move.l  a2,a0
        move.l  #T_8SVX,d0
        move.l  #ID_VHDR,d1
        jsr     IL_FindProp(a6)
        move.l  d0,a0
        bne.s   .r_fp
        lea     s_ffprop(pc),a1
        bra     do_fail
.r_fp:
        cmp.l   #20,(a0)                ; sp_Size
        beq.s   .r_spsz
        lea     s_fspsz(pc),a1
        bra     do_fail
.r_spsz:
        move.l  4(a0),a0                ; sp_Data
        move.l  (a0),d0
        cmp.l   vhdr_data,d0
        beq.s   .r_spd
        lea     s_fspd(pc),a1
        bra     do_fail
.r_spd:
        ; ReadChunkBytes(iff, rdbuf, 8) — BODY payload
        move.l  a2,a0
        lea     rdbuf(pc),a1
        moveq   #8,d0
        jsr     IL_ReadChunkBytes(a6)
        cmp.l   #8,d0
        beq.s   .r_rb
        lea     s_frdb(pc),a1
        bra     do_fail
.r_rb:
        move.l  rdbuf,d0
        cmp.l   body_data,d0
        beq.s   .r_rbd
        lea     s_frbd(pc),a1
        bra     do_fail
.r_rbd:
        move.l  a2,a0
        jsr     IL_CloseIFF(a6)
        move.l  a2,a0
        jsr     IL_FreeIFF(a6)
        movea.l dos_base,a6
        move.l  d7,d1
        jsr     LVO_Close(a6)

; ======================== rawstep phase ===============================
        lea     s_rs(pc),a1
        bsr     print

        movea.l dos_base,a6
        lea     file_name(pc),a0
        move.l  a0,d1
        move.l  #MODE_OLDFILE,d2
        jsr     LVO_Open(a6)
        move.l  d0,d7
        bne.s   .s_open
        lea     s_fropen(pc),a1
        bra     do_fail
.s_open:
        movea.l iff_base,a6
        jsr     IL_AllocIFF(a6)
        move.l  d0,a2
        bne.s   .s_iff
        lea     s_falloc(pc),a1
        bra     do_fail
.s_iff:
        move.l  d7,(a2)
        move.l  a2,a0
        jsr     IL_InitIFFasDOS(a6)
        move.l  a2,a0
        moveq   #IFFF_READ,d0
        jsr     IL_OpenIFF(a6)
        tst.l   d0
        beq.s   .s_opened
        lea     s_friff(pc),a1
        bra     do_fail
.s_opened:
        ; walk with RAWSTEP: expected sequence 0,0,-3,0,-3,-3,-2
        lea     exp_seq(pc),a3
        moveq   #6,d6                   ; 7 entries
.s_loop:
        move.l  a2,a0
        moveq   #IFFPARSE_RAWSTEP,d0
        jsr     IL_ParseIFF(a6)
        cmp.l   (a3)+,d0
        beq.s   .s_next
        lea     s_fseq(pc),a1
        bra     do_fail
.s_next:
        dbra    d6,.s_loop
        move.l  a2,a0
        jsr     IL_CloseIFF(a6)
        move.l  a2,a0
        jsr     IL_FreeIFF(a6)
        movea.l dos_base,a6
        move.l  d7,d1
        jsr     LVO_Close(a6)

        lea     s_pass(pc),a1
        bsr     print
        moveq   #0,d0
        rts

do_fail:
        bsr     print
        moveq   #10,d0                  ; RETURN_WARN — failure visible
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
iff_base:   dc.l 0

; 20-byte VHDR: oneshots, repeats, samples/sec, octaves, compression,
; volume — contents arbitrary, first long $11223344 for prop compare.
vhdr_data:  dc.l $11223344,$55667788,$99AABBCC,$DDEEFF00,$12345678
body_data:  dc.l $A1B2C3D4,$E5F60718

exp_seq:    dc.l 0,0,-3,0,-3,-3,-2

file_name:  dc.b "RAM:ifftest.8svx",0
dos_name:   dc.b "dos.library",0
iff_name:   dc.b "iffparse.library",0

s_start:    dc.b "IFFTEST begin",10,0
s_wr:       dc.b " write...",10,0
s_rd:       dc.b " read...",10,0
s_rs:       dc.b " rawstep...",10,0
s_pass:     dc.b "IFFTEST PASS",10,0

s_flib:     dc.b "IFFTEST FAIL OpenLibrary(iffparse)",10,0
s_fwopen:   dc.b "IFFTEST FAIL dos Open(NEWFILE)",10,0
s_fropen:   dc.b "IFFTEST FAIL dos Open(OLDFILE)",10,0
s_falloc:   dc.b "IFFTEST FAIL AllocIFF",10,0
s_fwiff:    dc.b "IFFTEST FAIL OpenIFF(WRITE)",10,0
s_friff:    dc.b "IFFTEST FAIL OpenIFF(READ)",10,0
s_fpush:    dc.b "IFFTEST FAIL PushChunk",10,0
s_fwrch:    dc.b "IFFTEST FAIL WriteChunkBytes",10,0
s_fpop:     dc.b "IFFTEST FAIL PopChunk",10,0
s_fprop:    dc.b "IFFTEST FAIL PropChunk",10,0
s_fstopc:   dc.b "IFFTEST FAIL StopChunk",10,0
s_fparse:   dc.b "IFFTEST FAIL ParseIFF(SCAN)",10,0
s_fcc:      dc.b "IFFTEST FAIL CurrentChunk NULL",10,0
s_fccid:    dc.b "IFFTEST FAIL CurrentChunk id != BODY",10,0
s_fcct:     dc.b "IFFTEST FAIL CurrentChunk type != 8SVX",10,0
s_ffprop:   dc.b "IFFTEST FAIL FindProp NULL",10,0
s_fspsz:    dc.b "IFFTEST FAIL sp_Size != 20",10,0
s_fspd:     dc.b "IFFTEST FAIL sp_Data mismatch",10,0
s_frdb:     dc.b "IFFTEST FAIL ReadChunkBytes != 8",10,0
s_frbd:     dc.b "IFFTEST FAIL BODY data mismatch",10,0
s_fseq:     dc.b "IFFTEST FAIL rawstep sequence",10,0

rdbuf:      ds.b 16

        end
