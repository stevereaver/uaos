; LibLoadTest.s — UAOS-305 acceptance: real hunk-format .library loading.
;
; Exercises exec.library/OpenLibrary resolving LIBS:uaostest.library —
; a genuine Amiga hunk-format library (RomTag + RTF_AUTOINIT InitTable,
; word-offset function table, InitStruct data table, code/data/bss hunks):
;
;   OpenLibrary -> LoadSeg -> RomTag scan -> InitResident -> MakeLibrary
;   -> init routine -> AddLibrary -> Open vector
;
; Checks (in order):
;   OpenLibrary("uaostest.library",99) == 0  (fresh-load version gate)
;   OpenLibrary("uaostest.library",1) != 0
;   FindName(&SysBase->LibList, name) == base
;   lib_Version == 3, lib_Revision == 1, lib_OpenCnt == 1
;   Add(2,40) == 42            (-30)
;   GetMagic == 'INIT'         (-36, init routine ran)
;   seglist at base+34 != 0
;   ExecProbe == 1             (-42, nested exec AllocMem/FreeMem)
;   GetData == $C0DEF00D       (-48, cross-hunk RELOC32 into DATA hunk)
;   Counter -> 1 then 2        (-54, BSS hunk)
;   reopen == same base, OpenCnt == 2
;   CloseLibrary -> OpenCnt == 1
;   OpenLibrary(name,99) == 0  (version gate)
;   OpenLibrary("LIBS:name",1) == same base (path normalisation)
;
; Prints "LIBLOAD PASS" or "LIBLOAD FAIL <stage>".  Then a non-fatal
; INFO block: for version.library / xpkNONE.library, if OpenLibrary
; returns a real disk library (base outside the genlib arena, matching
; ln_Name) it prints "LIBLOAD INFO <name> v<ver> <idstring>".

LVO_OpenLibrary  equ -552
LVO_CloseLibrary equ -414
LVO_FindName     equ -276
LVO_PutStr       equ -948
LV_RawPutChar    equ -516

SB_LIBLIST   equ $17A

; uaostest.library vectors
TL_Add       equ -30
TL_GetMagic  equ -36
TL_ExecProbe equ -42
TL_GetData   equ -48
TL_Counter   equ -54

; generated-lib arena — fake/ROM-bound bases; a base inside it can never
; be a disk-loaded library
GENLIB_LO    equ $7F0000
GENLIB_HI    equ $800000

        section code,code

start:
        movea.l 4.w,a6
        move.l  a6,exec_base

        lea     dos_name(pc),a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,dos_base
        beq     .early_exit
        lea     s_start(pc),a1
        bsr     print

; ---- 0. OpenLibrary("uaostest.library",99) == 0 ---------------------------
; Fresh-load version gate: this is the first touch of the library, so the
; loader LoadSegs it, InitResidents it, then refuses the open — the guest
; Open vector must NOT run (OpenCnt stays 0) and no fake base may result.
        movea.l exec_base,a6
        lea     lt_name(pc),a1
        move.l  #99,d0
        jsr     LVO_OpenLibrary(a6)
        tst.l   d0
        beq.s   .v99f_ok
        lea     s_fver99f(pc),a1
        bra     do_fail
.v99f_ok:

; ---- 1. OpenLibrary("uaostest.library",1) -------------------------------
        movea.l exec_base,a6
        lea     lt_name(pc),a1
        moveq   #1,d0
        jsr     LVO_OpenLibrary(a6)
        tst.l   d0
        bne.s   .o1_ok
        lea     s_fopen(pc),a1
        bra     do_fail
.o1_ok:
        move.l  d0,a4                   ; a4 = lib base

; ---- 2. FindName(SysBase.LibList, name) == base --------------------------
        movea.l exec_base,a6
        move.l  a6,a0
        add.l   #SB_LIBLIST,a0
        lea     lt_name(pc),a1
        jsr     LVO_FindName(a6)
        cmp.l   a4,d0
        beq.s   .fn_ok
        lea     s_ffind(pc),a1
        bra     do_fail
.fn_ok:

; ---- 3. lib_Version == 3, lib_Revision == 1 ------------------------------
        cmp.w   #3,20(a4)
        beq.s   .v_ok
        lea     s_fver(pc),a1
        bra     do_fail
.v_ok:
        cmp.w   #1,22(a4)
        beq.s   .r_ok
        lea     s_frev(pc),a1
        bra     do_fail
.r_ok:

; ---- 4. lib_OpenCnt == 1 --------------------------------------------------
        cmp.w   #1,32(a4)
        beq.s   .oc_ok
        lea     s_focnt(pc),a1
        bra     do_fail
.oc_ok:

; ---- 5. Add(2,40) == 42 ----------------------------------------------------
        movea.l a4,a6
        moveq   #2,d0
        move.l  #40,d1
        jsr     TL_Add(a6)
        cmp.l   #42,d0
        beq.s   .add_ok
        lea     s_fadd(pc),a1
        bra     do_fail
.add_ok:

; ---- 6. GetMagic == 'INIT' --------------------------------------------------
        jsr     TL_GetMagic(a6)
        cmp.l   #$494e4954,d0
        beq.s   .gm_ok
        lea     s_fmagic(pc),a1
        bra     do_fail
.gm_ok:

; ---- 7. seglist recorded at base+34 -----------------------------------------
        tst.l   34(a4)
        bne.s   .sg_ok
        lea     s_fseg(pc),a1
        bra     do_fail
.sg_ok:

; ---- 8. ExecProbe == 1 --------------------------------------------------------
        jsr     TL_ExecProbe(a6)
        cmp.l   #1,d0
        beq.s   .ep_ok
        lea     s_fprobe(pc),a1
        bra     do_fail
.ep_ok:

; ---- 9. GetData == $C0DEF00D ---------------------------------------------------
        jsr     TL_GetData(a6)
        cmp.l   #$c0def00d,d0
        beq.s   .gd_ok
        lea     s_fdata(pc),a1
        bra     do_fail
.gd_ok:

; ---- 10. Counter -> 1, then 2 ---------------------------------------------------
        jsr     TL_Counter(a6)
        cmp.l   #1,d0
        beq.s   .c1_ok
        lea     s_fcnt1(pc),a1
        bra     do_fail
.c1_ok:
        jsr     TL_Counter(a6)
        cmp.l   #2,d0
        beq.s   .c2_ok
        lea     s_fcnt2(pc),a1
        bra     do_fail
.c2_ok:

; ---- 11. reopen -> same base, OpenCnt == 2 ---------------------------------------
        movea.l exec_base,a6
        lea     lt_name(pc),a1
        moveq   #1,d0
        jsr     LVO_OpenLibrary(a6)
        cmp.l   a4,d0
        beq.s   .o2_ok
        lea     s_fopen2(pc),a1
        bra     do_fail
.o2_ok:
        cmp.w   #2,32(a4)
        beq.s   .oc2_ok
        lea     s_focnt2(pc),a1
        bra     do_fail
.oc2_ok:

; ---- 12. CloseLibrary -> OpenCnt == 1 ----------------------------------------------
        movea.l exec_base,a6
        move.l  a4,a1
        jsr     LVO_CloseLibrary(a6)
        cmp.w   #1,32(a4)
        beq.s   .cl_ok
        lea     s_fclose(pc),a1
        bra     do_fail
.cl_ok:

; ---- 13. version gate: OpenLibrary(name,99) == 0 --------------------------------------
        movea.l exec_base,a6
        lea     lt_name(pc),a1
        move.l  #99,d0
        jsr     LVO_OpenLibrary(a6)
        tst.l   d0
        beq.s   .vg_ok
        lea     s_fver99(pc),a1
        bra     do_fail
.vg_ok:

; ---- 14. "LIBS:uaostest.library" resolves to the same base ------------------------------
        movea.l exec_base,a6
        lea     lt_path(pc),a1
        moveq   #1,d0
        jsr     LVO_OpenLibrary(a6)
        cmp.l   a4,d0
        beq.s   .lp_ok
        lea     s_flibs(pc),a1
        bra     do_fail
.lp_ok:
        ; leave OpenCnt where it started
        movea.l exec_base,a6
        move.l  a4,a1
        jsr     LVO_CloseLibrary(a6)

        lea     s_pass(pc),a1
        bsr     print

; ---- INFO (non-fatal): report real disk libs if present ------------------------------
        lea     info_ver(pc),a0
        bsr     info_check
        lea     info_xpk(pc),a0
        bsr     info_check

        moveq   #0,d0
        rts
.early_exit:
        moveq   #10,d0
        rts

do_fail:
        bsr     print
        moveq   #10,d0
        rts

; ---------------------------------------------------------------------
; helpers
; ---------------------------------------------------------------------

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

print_dec:                              ; d0 = unsigned word value
        movem.l d0-d3/a1-a2/a6,-(sp)
        lea     decbuf+15(pc),a2
        move.b  #0,-(a2)
.pd_div:
        divu    #10,d0
        swap    d0
        move.b  d0,d2
        add.b   #'0',d2
        move.b  d2,-(a2)
        clr.w   d0
        swap    d0
        tst.l   d0
        bne.s   .pd_div
        move.l  a2,a1
        bsr     print
        movem.l (sp)+,d0-d3/a1-a2/a6
        rts

strcmp:                                 ; a0 vs a1 -> d0 = 1 equal, 0 not
        moveq   #1,d0
.sc_lp: move.b  (a0)+,d1
        cmp.b   (a1)+,d1
        bne.s   .sc_ne
        tst.b   d1
        bne.s   .sc_lp
        rts
.sc_ne: moveq   #0,d0
        rts

; info_check(a0 = lib name): OpenLibrary(name,0); if the returned base is
; a real disk library (outside the genlib arena, ln_Name == name) print
; "LIBLOAD INFO <name> v<ver> <idstring>"; CloseLibrary either way.
; Never fails the test.
info_check:
        movem.l d0-d2/a0-a4/a6,-(sp)
        move.l  a0,a3                   ; a3 = name
        movea.l exec_base,a6
        move.l  a3,a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        tst.l   d0
        beq     .ic_done
        move.l  d0,a4                   ; a4 = base
        ; generated/fake bases live in the 0x7F0000-0x800000 arena;
        ; a real disk lib is heap-loaded outside it
        cmp.l   #GENLIB_LO,a4
        blo.s   .ic_maybe               ; below arena -> could be real
        cmp.l   #GENLIB_HI,a4
        blo.s   .ic_close               ; inside arena -> fake genlib
.ic_maybe:
        move.l  10(a4),a1               ; ln_Name
        move.l  a1,d0
        beq.s   .ic_close
        move.l  a3,a0
        bsr     strcmp
        tst.l   d0
        beq.s   .ic_close
        lea     s_info(pc),a1
        bsr     print
        move.l  a3,a1
        bsr     print
        lea     s_infov(pc),a1
        bsr     print
        move.w  20(a4),d0
        bsr     print_dec
        lea     s_infos(pc),a1
        bsr     print
        move.l  24(a4),a1               ; lib_IdString
        bsr     print
        lea     s_infe(pc),a1
        bsr     print
.ic_close:
        movea.l exec_base,a6
        move.l  a4,a1
        jsr     LVO_CloseLibrary(a6)
.ic_done:
        movem.l (sp)+,d0-d2/a0-a4/a6
        rts

; ---- data ------------------------------------------------------------

        even
exec_base:  dc.l 0
dos_base:   dc.l 0

dos_name:   dc.b "dos.library",0
lt_name:    dc.b "uaostest.library",0
lt_path:    dc.b "LIBS:uaostest.library",0
info_ver:   dc.b "version.library",0
info_xpk:   dc.b "xpkNONE.library",0

s_start:    dc.b "LIBLOAD begin",10,0
s_pass:     dc.b "LIBLOAD PASS",10,0

s_fopen:    dc.b "LIBLOAD FAIL OpenLibrary NULL",10,0
s_ffind:    dc.b "LIBLOAD FAIL LibList FindName",10,0
s_fver:     dc.b "LIBLOAD FAIL lib_Version",10,0
s_frev:     dc.b "LIBLOAD FAIL lib_Revision",10,0
s_focnt:    dc.b "LIBLOAD FAIL lib_OpenCnt",10,0
s_fadd:     dc.b "LIBLOAD FAIL Add",10,0
s_fmagic:   dc.b "LIBLOAD FAIL GetMagic (init not run)",10,0
s_fseg:     dc.b "LIBLOAD FAIL seglist",10,0
s_fprobe:   dc.b "LIBLOAD FAIL ExecProbe",10,0
s_fdata:    dc.b "LIBLOAD FAIL GetData",10,0
s_fcnt1:    dc.b "LIBLOAD FAIL Counter#1",10,0
s_fcnt2:    dc.b "LIBLOAD FAIL Counter#2",10,0
s_fopen2:   dc.b "LIBLOAD FAIL reopen base",10,0
s_focnt2:   dc.b "LIBLOAD FAIL reopen OpenCnt",10,0
s_fclose:   dc.b "LIBLOAD FAIL CloseLibrary",10,0
s_fver99f:  dc.b "LIBLOAD FAIL fresh v99 open",10,0
s_fver99:   dc.b "LIBLOAD FAIL version gate",10,0
s_flibs:    dc.b "LIBLOAD FAIL LIBS: path",10,0

s_info:     dc.b "LIBLOAD INFO ",0
s_infov:    dc.b " v",0
s_infos:    dc.b " ",0
s_infe:     dc.b 10,0

        even
decbuf:     ds.b 16

        end
