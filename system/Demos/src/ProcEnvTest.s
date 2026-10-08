; ProcEnvTest.s — UAOS-237 acceptance test for the guest Process environment.
;
; Verifies the real Process/Task/CLI block, console port, seglist linkage
; and library headers:
;   1. FindTask(NULL)        -> NT_PROCESS node, sane stack/Task fields
;   2. pr_MsgPort            -> NT_MSGPORT, mp_SigTask = this process
;   3. pr_CIS/pr_COS         -> fake stdin/stdout BPTRs ($101/$100)
;   4. pr_ConsoleTask        -> NT_MSGPORT owned by this process; matches
;                              dos.library/GetConsoleTask()
;   5. pr_CLI/cli fields     -> cli_CommandName, cli_Module == pr_SegList,
;                              cli_Interactive, default input
;   6. dos.Cli()             -> TRUE
;   7. Library headers       -> OpenLibrary() bases carry NT_LIBRARY,
;                              ln_Name, lib_Version >= 37; unknown libs get
;                              a generated versioned base
;   8. FindName(LibList)     -> dos.library resolves through the real list
;
; Prints "PROCENV PASS" or "PROCENV FAIL xx" (hex test id) on stdout.

LVO_FindTask        equ -294
LVO_FindName        equ -276
LVO_OpenLibrary     equ -552
LVO_DOS_Cli         equ -492
LVO_DOS_GetConTask  equ -510
LVO_DOS_PutStr      equ -948

EXECBASE_LIBLIST    equ $17A
DOS_BASE            equ $2000

        section code,code

start:
        movea.l 4.w,a6
        move.l  a6,exec_base

        ; ---- open dos.library early (PutStr) --------------------------
        lea     dos_name(pc),a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,dos_base
        bne.s   .dos_ok
        rts
.dos_ok:

        ; ---- 1. FindTask(NULL) -> a2 = Process ------------------------
        movea.l exec_base,a6
        suba.l  a1,a1
        jsr     LVO_FindTask(a6)
        move.l  d0,proc
        bne.s   .have_proc
        moveq   #1,d7
        bra     do_fail
.have_proc:
        movea.l proc,a2

        ; ln_Type == NT_PROCESS (13)
        cmp.b   #13,8(a2)
        beq.s   .t2
        moveq   #2,d7
        bra     do_fail
.t2:
        ; ln_Name != 0 and readable (first char printable ASCII)
        move.l  10(a2),d0
        beq     .f03
        movea.l d0,a0
        cmp.b   #$20,(a0)
        bhs.s   .t4
.f03:   moveq   #3,d7
        bra     do_fail
.t4:
        ; tc_State == TS_RUN (2)
        cmp.b   #2,$0F(a2)
        beq.s   .t5
        moveq   #4,d7
        bra     do_fail
.t5:
        ; tc_SPUpper > tc_SPLower (stack bounds sane)
        move.l  $3A(a2),d0
        cmp.l   $3E(a2),d0
        blo.s   .t6
        moveq   #5,d7
        bra     do_fail
.t6:
        ; pr_MsgPort (+$5C): ln_Type == NT_MSGPORT (4), mp_SigTask == proc
        cmp.b   #4,$64(a2)
        beq.s   .t7
        moveq   #6,d7
        bra     do_fail
.t7:
        move.l  $6C(a2),d0
        cmp.l   proc,d0
        beq.s   .t8
        moveq   #7,d7
        bra     do_fail
.t8:
        ; pr_CIS == $101, pr_COS == $100
        cmp.l   #$101,$9C(a2)
        beq.s   .t9
        moveq   #8,d7
        bra     do_fail
.t9:
        cmp.l   #$100,$A0(a2)
        beq.s   .t10
        moveq   #9,d7
        bra     do_fail
.t10:
        ; pr_ConsoleTask != 0 -> a3 = port; NT_MSGPORT + owned by proc
        move.l  $A4(a2),conport
        bne.s   .t11
        moveq   #10,d7
        bra     do_fail
.t11:
        movea.l conport,a3
        cmp.b   #4,8(a3)
        beq.s   .t12
        moveq   #11,d7
        bra     do_fail
.t12:
        move.l  $10(a3),d0
        cmp.l   proc,d0
        beq.s   .t13
        moveq   #12,d7
        bra     do_fail
.t13:
        ; pr_SegList != 0; link field resolves inside guest RAM (< $1000000)
        move.l  $80(a2),seglist
        bne.s   .t14
        moveq   #13,d7
        bra     do_fail
.t14:
        move.l  seglist,d0
        lsl.l   #2,d0
        cmp.l   #$1000000,d0
        blo.s   .t15
        moveq   #14,d7
        bra     do_fail
.t15:
        ; pr_StackSize != 0, pr_GlobVec != 0
        tst.l   $84(a2)
        bne.s   .t16
        moveq   #15,d7
        bra     do_fail
.t16:
        tst.l   $88(a2)
        bne.s   .t17
        moveq   #16,d7
        bra     do_fail
.t17:
        ; pr_CLI != 0 -> a3 = cli (BPTR<<2)
        move.l  $AC(a2),d0
        bne.s   .t18
        moveq   #17,d7
        bra     do_fail
.t18:
        lsl.l   #2,d0
        movea.l d0,a3
        move.l  a3,cli
        ; cli_CommandName (+$10) != 0
        tst.l   $10(a3)
        bne.s   .t19
        moveq   #18,d7
        bra     do_fail
.t19:
        ; cli_Module (+$3C) == pr_SegList
        move.l  $3C(a3),d0
        cmp.l   seglist,d0
        beq.s   .t20
        moveq   #19,d7
        bra     do_fail
.t20:
        ; cli_DefaultInput (+$1C) == $101, cli_CurrentOutput (+$30) == $100
        cmp.l   #$101,$1C(a3)
        beq.s   .t21
        moveq   #20,d7
        bra     do_fail
.t21:
        cmp.l   #$100,$30(a3)
        beq.s   .t22
        moveq   #21,d7
        bra     do_fail
.t22:
        ; cli_Interactive (+$44) != 0
        tst.l   $44(a3)
        bne.s   .t23
        moveq   #22,d7
        bra     do_fail
.t23:
        ; ---- dos.Cli() == TRUE ----------------------------------------
        movea.l dos_base,a6
        jsr     LVO_DOS_Cli(a6)
        tst.l   d0
        bne.s   .t24
        moveq   #23,d7
        bra     do_fail
.t24:
        ; ---- dos.GetConsoleTask() == pr_ConsoleTask --------------------
        movea.l dos_base,a6
        jsr     LVO_DOS_GetConTask(a6)
        cmp.l   conport,d0
        beq.s   .t25
        moveq   #24,d7
        bra     do_fail
.t25:
        ; ---- OpenLibrary("graphics.library",37) header ----------------
        movea.l exec_base,a6
        lea     gfx_name(pc),a1
        moveq   #37,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,a3
        bne.s   .t26
        moveq   #25,d7
        bra     do_fail
.t26:
        cmp.b   #9,8(a3)               ; ln_Type == NT_LIBRARY
        beq.s   .t27
        moveq   #26,d7
        bra     do_fail
.t27:
        cmp.w   #37,20(a3)             ; lib_Version >= 37
        bge.s   .t28
        moveq   #27,d7
        bra     do_fail
.t28:
        move.l  10(a3),a0              ; ln_Name == "graphics.library"
        lea     gfx_name(pc),a1
        bsr     strcmp
        tst.l   d0
        beq.s   .t29
        moveq   #28,d7
        bra     do_fail
.t29:
        ; ---- OpenLibrary("bogus.library",37) -> generated versioned base
        movea.l exec_base,a6
        lea     bogus_name(pc),a1
        moveq   #37,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,a3
        bne.s   .t30
        moveq   #29,d7
        bra     do_fail
.t30:
        cmp.b   #9,8(a3)
        beq.s   .t31
        moveq   #30,d7
        bra     do_fail
.t31:
        cmp.w   #37,20(a3)
        bge.s   .t32
        moveq   #31,d7
        bra     do_fail
.t32:
        move.l  10(a3),a0
        lea     bogus_name(pc),a1
        bsr     strcmp
        tst.l   d0
        beq.s   .t33
        moveq   #32,d7
        bra     do_fail
.t33:
        ; ---- FindName(&SysBase->LibList, "dos.library") == DOS_BASE ----
        movea.l exec_base,a0
        lea     EXECBASE_LIBLIST(a0),a0
        lea     dos_name(pc),a1
        movea.l exec_base,a6
        jsr     LVO_FindName(a6)
        cmp.l   #DOS_BASE,d0
        beq.s   .pass
        moveq   #33,d7
        bra     do_fail

.pass:
        lea     s_pass(pc),a1
        bsr     print
        rts

do_fail:                               ; d7 = test id -> "PROCENV FAIL xx"
        move.l  d7,d6
        lea     s_fail(pc),a1
        bsr     print
        move.l  d6,d0
        lsl.l   #8,d0               ; low byte to top for rol-nibble walk
        lsl.l   #8,d0
        lsl.l   #8,d0
        lea     numbuf,a0
        moveq   #1,d1               ; two hex digits
.hx:    rol.l   #4,d0
        move.b  d0,d2
        and.b   #$0F,d2
        cmp.b   #10,d2
        blt.s   .dig
        add.b   #'A'-10-'0',d2
.dig:   add.b   #'0',d2
        move.b  d2,(a0)+
        dbra    d1,.hx
        move.b  #10,(a0)
        move.b  #0,(a0)+
        lea     numbuf,a1
        bsr     print
        rts

; ---- helpers ---------------------------------------------------------

print:                              ; a1 = NUL-terminated string
        movem.l d1/a6,-(sp)
        move.l  a1,d1
        movea.l dos_base,a6
        jsr     LVO_DOS_PutStr(a6)
        movem.l (sp)+,d1/a6
        rts

strcmp:                             ; a0, a1 C strings -> d0 = 0 equal
        moveq   #1,d0
.l:     move.b  (a0)+,d1
        cmp.b   (a1)+,d1
        bne.s   .r
        tst.b   d1
        bne.s   .l
        moveq   #0,d0
.r:     rts

; ---- data ------------------------------------------------------------

        even
exec_base:  dc.l 0
dos_base:   dc.l 0
proc:       dc.l 0
cli:        dc.l 0
conport:    dc.l 0
seglist:    dc.l 0

dos_name:   dc.b "dos.library",0
gfx_name:   dc.b "graphics.library",0
bogus_name: dc.b "bogus.library",0
s_pass:     dc.b "PROCENV PASS",10,0
s_fail:     dc.b "PROCENV FAIL ",0

numbuf:     ds.b 8

        end
