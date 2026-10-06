; ASLTest.s — UAOS-242 asl.library acceptance test.
;
; Phase 1 (multi-select): AllocAslRequest(FileRequest, {DoMultiSelect,
;          DoPatterns, InitialDrawer, InitialPattern}) + AslRequest.
;          Operator marks 2+ files and clicks OK.  The test dumps
;          fr_Drawer/fr_File/fr_NumArgs/fr_Pattern and then opens every
;          fr_ArgList WBArg entry relative to its wa_Lock — the serial
;          [dos] Open('<path>') lines prove the WBArg list resolves.
; Phase 2 (save mode):  requester with ASLFR_DoSaveMode + InitialFile;
;          operator types/edits a name and clicks OK.  The result is
;          created via Open(NEWFILE)+Write on the drawer+file path.
; Phase 3 (delete):     plain requester with InitialDrawer RAM: +
;          InitialFile "ASLDEL.TMP" (pre-created); operator clicks OK —
;          the returned drawer+file path is passed to DeleteFile.
;          Sentinel RAM:ASLDOK / RAM:ASLDELFAIL.
; Phase 4 (cancel):     plain requester; operator clicks Cancel —
;          AslRequest must return FALSE.
;
; Prints "ASLTEST PASS" on stdout when all three phases complete.

LVO_OpenLibrary equ -552
LVO_CloseLibrary equ -414
LVO_PutStr     equ -948
LVO_Open       equ -30
LVO_Close      equ -36
LVO_Write      equ -48
LVO_CurrentDir equ -126
LVO_AddPart    equ -882
LVO_DeleteFile equ -72

ASL_Alloc      equ -48               ; AllocAslRequest(type=d0, tags=a0)
ASL_Free       equ -54               ; FreeAslRequest(req=a0)
ASL_Request    equ -60               ; AslRequest(req=a0, tags=a1)

ASL_TB              equ $80080000
ASL_Hail            equ ASL_TB+1
ASL_InitialFile     equ ASL_TB+8
ASL_InitialDrawer   equ ASL_TB+9
ASL_InitialPattern  equ ASL_TB+10
ASLFR_DoSaveMode    equ ASL_TB+44
ASLFR_DoMultiSelect equ ASL_TB+45
ASLFR_DoPatterns    equ ASL_TB+46

FR_FILE     equ 4
FR_DRAWER   equ 8
FR_NUMARGS  equ 32
FR_ARGLIST  equ 36
FR_PATTERN  equ 52

MODE_OLDFILE equ 1005
MODE_NEWFILE equ 1006

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
        lea     asl_name(pc),a1
        moveq   #37,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,asl_base
        bne.s   .asl_ok
        lea     s_flib(pc),a1
        bra     do_fail
.asl_ok:

; ====================== phase 1: multi-select =========================
        lea     s_ph1(pc),a1
        bsr     print

        movea.l asl_base,a6
        moveq   #0,d0                   ; ASL_FileRequest
        lea     tags_multi(pc),a0
        jsr     ASL_Alloc(a6)
        move.l  d0,req
        bne.s   .m_alloc
        lea     s_falloc(pc),a1
        bra     do_fail
.m_alloc:
        movea.l asl_base,a6
        movea.l req,a0
        suba.l  a1,a1                   ; no request-time tags
        jsr     ASL_Request(a6)
        tst.l   d0
        bne.s   .m_reqok
        lea     s_freq1(pc),a1
        bra     do_fail
.m_reqok:
        movea.l req,a2

        lea     s_drawer(pc),a1
        bsr     print
        movea.l FR_DRAWER(a2),a1
        cmpa.l  #0,a1
        beq.s   .m_nodrawer
        bsr     print
.m_nodrawer:
        lea     s_nl(pc),a1
        bsr     print

        lea     s_file(pc),a1
        bsr     print
        movea.l FR_FILE(a2),a1
        cmpa.l  #0,a1
        beq.s   .m_nofile
        bsr     print
.m_nofile:
        lea     s_nl(pc),a1
        bsr     print

        lea     s_pat(pc),a1
        bsr     print
        movea.l FR_PATTERN(a2),a1
        cmpa.l  #0,a1
        beq.s   .m_nopat
        bsr     print
.m_nopat:
        lea     s_nl(pc),a1
        bsr     print

        lea     s_nargs(pc),a1
        bsr     print
        move.l  FR_NUMARGS(a2),d0
        bsr     print_dec
        lea     s_nl(pc),a1
        bsr     print

        ; walk fr_ArgList: per WBArg {wa_Lock, wa_Name} — open the file
        ; relative to the lock so serial shows the resolved path.
        move.l  FR_NUMARGS(a2),d7
        movea.l FR_ARGLIST(a2),a3
.m_argloop:
        tst.l   d7
        beq     .m_argdone

        movea.l dos_base,a6
        move.l  (a3),d1                 ; wa_Lock → make it current dir
        jsr     LVO_CurrentDir(a6)
        move.l  d0,oldlock

        movea.l dos_base,a6
        move.l  4(a3),d1                ; wa_Name
        move.l  #MODE_OLDFILE,d2
        jsr     LVO_Open(a6)
        move.l  d0,fh

        lea     s_arg(pc),a1
        bsr     print
        movea.l 4(a3),a1
        bsr     print
        lea     s_openres(pc),a1
        bsr     print
        move.l  fh,d0
        bsr     print_dec
        lea     s_nl(pc),a1
        bsr     print

        move.l  fh,d0
        beq.s   .m_nofh
        movea.l dos_base,a6
        move.l  d0,d1
        jsr     LVO_Close(a6)
.m_nofh:
        movea.l dos_base,a6
        move.l  oldlock,d1
        jsr     LVO_CurrentDir(a6)

        addq.l  #8,a3
        subq.l  #1,d7
        bra     .m_argloop
.m_argdone:
        movea.l asl_base,a6
        movea.l req,a0
        jsr     ASL_Free(a6)
        lea     mark_p1(pc),a1
        bsr     mkfile                   ; serial sentinel: phase1 done

; ====================== phase 2: save mode ============================
        lea     s_ph2(pc),a1
        bsr     print

        movea.l asl_base,a6
        moveq   #0,d0
        lea     tags_save(pc),a0
        jsr     ASL_Alloc(a6)
        move.l  d0,req
        bne.s   .s_alloc
        lea     s_falloc(pc),a1
        bra     do_fail
.s_alloc:
        movea.l asl_base,a6
        movea.l req,a0
        suba.l  a1,a1
        jsr     ASL_Request(a6)
        tst.l   d0
        bne.s   .s_reqok
        lea     s_freq2(pc),a1
        bra     do_fail
.s_reqok:
        movea.l req,a2
        lea     s_saveres(pc),a1
        bsr     print
        movea.l FR_FILE(a2),a1
        bsr     print
        lea     s_nl(pc),a1
        bsr     print

        ; pathbuf = fr_Drawer; AddPart(pathbuf, fr_File, 160)
        movea.l FR_DRAWER(a2),a0
        lea     pathbuf(pc),a1
.s_cpyd:
        move.b  (a0)+,(a1)+
        bne.s   .s_cpyd
        movea.l dos_base,a6
        lea     pathbuf(pc),a0
        move.l  a0,d1
        move.l  FR_FILE(a2),d2
        move.l  #160,d3
        jsr     LVO_AddPart(a6)
        tst.l   d0
        bne.s   .s_addok
        lea     s_faddp(pc),a1
        bra     do_fail
.s_addok:
        movea.l dos_base,a6
        lea     pathbuf(pc),a0
        move.l  a0,d1
        move.l  #MODE_NEWFILE,d2
        jsr     LVO_Open(a6)
        move.l  d0,fh
        bne.s   .s_open
        lea     s_fwopen(pc),a1
        bra     do_fail
.s_open:
        movea.l dos_base,a6
        move.l  fh,d1
        lea     s_wdata(pc),a0
        move.l  a0,d2
        moveq   #s_wdata_len,d3
        jsr     LVO_Write(a6)
        movea.l dos_base,a6
        move.l  fh,d1
        jsr     LVO_Close(a6)
        lea     s_wrote(pc),a1
        bsr     print
        lea     pathbuf(pc),a1
        bsr     print
        lea     s_nl(pc),a1
        bsr     print

        movea.l asl_base,a6
        movea.l req,a0
        jsr     ASL_Free(a6)

; ====================== phase 3: delete ===============================
        lea     s_phd(pc),a1
        bsr     print
        lea     del_name(pc),a1
        bsr     mkfile                   ; create RAM:ASLDEL.TMP

        movea.l asl_base,a6
        moveq   #0,d0
        lea     tags_del(pc),a0
        jsr     ASL_Alloc(a6)
        move.l  d0,req
        bne.s   .d_alloc
        lea     s_falloc(pc),a1
        bra     do_fail
.d_alloc:
        movea.l asl_base,a6
        movea.l req,a0
        suba.l  a1,a1
        jsr     ASL_Request(a6)
        tst.l   d0
        bne.s   .d_reqok
        lea     s_freq3(pc),a1
        bra     do_fail
.d_reqok:
        movea.l req,a2
        ; pathbuf = fr_Drawer; AddPart(pathbuf, fr_File, 160)
        movea.l FR_DRAWER(a2),a0
        lea     pathbuf(pc),a1
.d_cpyd:
        move.b  (a0)+,(a1)+
        bne.s   .d_cpyd
        movea.l dos_base,a6
        lea     pathbuf(pc),a0
        move.l  a0,d1
        move.l  FR_FILE(a2),d2
        move.l  #160,d3
        jsr     LVO_AddPart(a6)
        movea.l dos_base,a6
        lea     pathbuf(pc),a0
        move.l  a0,d1
        jsr     LVO_DeleteFile(a6)
        tst.l   d0
        beq.s   .d_fail
        lea     mark_dok(pc),a1
        bra.s   .d_mark
.d_fail:
        lea     mark_dfail(pc),a1
.d_mark:
        bsr     mkfile
        movea.l asl_base,a6
        movea.l req,a0
        jsr     ASL_Free(a6)

; ====================== phase 4: cancel ===============================
        lea     s_ph3(pc),a1
        bsr     print

        movea.l asl_base,a6
        moveq   #0,d0
        lea     tags_plain(pc),a0
        jsr     ASL_Alloc(a6)
        move.l  d0,req
        bne.s   .c_alloc
        lea     s_falloc(pc),a1
        bra     do_fail
.c_alloc:
        movea.l asl_base,a6
        movea.l req,a0
        suba.l  a1,a1
        jsr     ASL_Request(a6)
        tst.l   d0
        beq.s   .c_cancelled
        bra     .c_badret
.c_cancelled:
        lea     s_cok(pc),a1
        bsr     print
        movea.l asl_base,a6
        movea.l req,a0
        jsr     ASL_Free(a6)
        lea     mark_cok(pc),a1
        bsr     mkfile                   ; serial sentinel: cancel ok
        bra     .done

.c_badret:
        lea     s_fcancel(pc),a1
        bsr     print
        lea     mark_cfail(pc),a1
        bsr     mkfile                   ; serial sentinel: cancel FAILED
        movea.l asl_base,a6
        movea.l req,a0
        jsr     ASL_Free(a6)
        moveq   #10,d0
        rts

.done:
        lea     s_pass(pc),a1
        bsr     print
        lea     mark_pass(pc),a1
        bsr     mkfile                   ; serial sentinel: ASLTEST PASS
        moveq   #0,d0
        rts

do_fail:
        bsr     print
        moveq   #10,d0
        rts

mkfile:                                  ; a1 = NUL filename — create+close
        movem.l d1-d3/a6,-(sp)
        movea.l dos_base,a6
        move.l  a1,d1
        move.l  #MODE_NEWFILE,d2
        jsr     LVO_Open(a6)
        tst.l   d0
        beq.s   .t_out
        move.l  d0,d1
        jsr     LVO_Close(a6)
.t_out:
        movem.l (sp)+,d1-d3/a6
        rts

print:                                  ; a1 = NUL-terminated string
        movem.l d1/a6,-(sp)
        move.l  a1,d1
        movea.l dos_base,a6
        jsr     LVO_PutStr(a6)
        movem.l (sp)+,d1/a6
        rts

print_dec:                              ; d0 = unsigned value
        movem.l d0-d2/a0-a1,-(sp)
        lea     numbuf+15(pc),a0
        clr.b   (a0)
.pd_loop:
        divu    #10,d0                  ; [rem:16][quot:16]
        swap    d0                      ; low word = remainder
        add.b   #'0',d0
        move.b  d0,-(a0)
        clr.w   d0
        swap    d0                      ; low word = quotient
        tst.l   d0
        bne.s   .pd_loop
        movea.l a0,a1
        bsr     print
        movem.l (sp)+,d0-d2/a0-a1
        rts

; ---- data ------------------------------------------------------------

        even
exec_base:  dc.l 0
dos_base:   dc.l 0
asl_base:   dc.l 0
req:        dc.l 0
fh:         dc.l 0
oldlock:    dc.l 0

tags_multi: dc.l ASL_Hail,title_multi
            dc.l ASL_InitialDrawer,drawer_name
            dc.l ASL_InitialPattern,pat_all
            dc.l ASLFR_DoMultiSelect,1
            dc.l ASLFR_DoPatterns,1
            dc.l 0,0
tags_save:  dc.l ASL_Hail,title_save
            dc.l ASLFR_DoSaveMode,1
            dc.l ASL_InitialFile,save_name
            dc.l 0,0
tags_del:   dc.l ASL_Hail,title_del
            dc.l ASL_InitialDrawer,drawer_ram
            dc.l ASL_InitialFile,del_base
            dc.l 0,0
tags_plain: dc.l ASL_Hail,title_cancel
            dc.l 0,0

dos_name:   dc.b "dos.library",0
asl_name:   dc.b "asl.library",0
drawer_name: dc.b "OCTAMED:",0
pat_all:    dc.b "#?",0
save_name:  dc.b "ASLTEST.SAV",0
title_multi: dc.b "ASLTEST multi-select",0
title_save:  dc.b "ASLTEST save",0
title_cancel: dc.b "ASLTEST cancel-me",0
s_wdata:    dc.b "asltest save ok",10
s_wdata_len equ *-s_wdata
mark_p1:    dc.b "RAM:ASLP1OK",0
mark_cok:   dc.b "RAM:ASLCOK",0
mark_cfail: dc.b "RAM:ASLCFAIL",0
mark_pass:  dc.b "RAM:ASLPASS",0
del_name:   dc.b "RAM:ASLDEL.TMP",0
drawer_ram: dc.b "RAM:",0
del_base:   dc.b "ASLDEL.TMP",0
title_del:  dc.b "ASLTEST delete",0
mark_dok:   dc.b "RAM:ASLDOK",0
mark_dfail: dc.b "RAM:ASLDELNG",0

s_start:    dc.b "ASLTEST begin",10,0
s_ph1:      dc.b "-- phase1: multi-select — mark files, click OK --",10,0
s_ph2:      dc.b "-- phase2: save — edit filename, click OK --",10,0
s_phd:      dc.b "-- phase3: delete — click OK --",10,0
s_ph3:      dc.b "-- phase4: click Cancel --",10,0
s_drawer:   dc.b "drawer='",0
s_file:     dc.b "file='",0
s_pat:      dc.b "pattern='",0
s_nargs:    dc.b "numargs=",0
s_arg:      dc.b "arg '",0
s_openres:  dc.b "' open->",0
s_saveres:  dc.b "save file='",0
s_wrote:    dc.b "wrote ",0
s_cok:      dc.b "cancel returned FALSE — ok",10,0
s_nl:       dc.b "'",10,0
s_pass:     dc.b "ASLTEST PASS",10,0

s_flib:     dc.b "ASLTEST FAIL OpenLibrary(asl)",10,0
s_falloc:   dc.b "ASLTEST FAIL AllocAslRequest",10,0
s_freq1:    dc.b "ASLTEST FAIL multi AslRequest cancelled",10,0
s_freq2:    dc.b "ASLTEST FAIL save AslRequest cancelled",10,0
s_fcancel:  dc.b "ASLTEST FAIL cancel returned TRUE",10,0
s_freq3:    dc.b "ASLTEST FAIL delete AslRequest cancelled",10,0
s_faddp:    dc.b "ASLTEST FAIL AddPart",10,0
s_fwopen:   dc.b "ASLTEST FAIL Open(NEWFILE)",10,0

pathbuf:    ds.b 160
numbuf:     ds.b 16

        end
