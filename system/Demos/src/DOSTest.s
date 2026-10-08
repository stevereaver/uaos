; DOSTest.s — UAOS-248 dos.library coverage test.
;
; Exercises the DOS functions OctaMED file flows need:
;   Lock/Examine/DupLock/ParentDir/CurrentDir through assigns (S:),
;   Info/IsFileSystem/DeviceProc, create/write/SetFileSize/Seek/Read
;   round-trip, ExamineFH/ParentOfFH/DupLockFromFH/OpenFromLock/SameLock,
;   SetComment/SetProtection/Rename/DeleteFile, CreateDir, ExAll/ExAllEnd,
;   Inhibit, AssignAdd.
;
; Prints "<name> PASS" or "<name> FAIL ioerr=N" per check.

LVO_OpenLibrary  equ -552
LVO_CloseLibrary equ -414

LVO_Open         equ -30
LVO_Close        equ -36
LVO_Read         equ -42
LVO_Write        equ -48
LVO_Seek         equ -66
LVO_DeleteFile   equ -72
LVO_Rename       equ -78
LVO_Lock         equ -84
LVO_UnLock       equ -90
LVO_DupLock      equ -96
LVO_Examine      equ -102
LVO_Info         equ -114
LVO_CreateDir    equ -120
LVO_CurrentDir   equ -126
LVO_IoErr        equ -132
LVO_DeviceProc   equ -174
LVO_SetComment   equ -180
LVO_SetProtect   equ -186
LVO_ParentDir    equ -210
LVO_DupLockFromFH equ -372
LVO_OpenFromLock equ -378
LVO_ParentOfFH   equ -384
LVO_ExamineFH    equ -390
LVO_SameLock     equ -420
LVO_ExAll        equ -432
LVO_SetFileSize  equ -456
LVO_AssignAdd    equ -630
LVO_IsFileSystem equ -708
LVO_Inhibit      equ -726
LVO_PutStr       equ -948
LVO_ExAllEnd     equ -990

MODE_OLDFILE     equ 1005
MODE_NEWFILE     equ 1006
SHARED_LOCK      equ -2
OFFSET_BEGINNING equ -1

fib_DirEntryType equ 4
fib_Size         equ 124
FIB_SIZE         equ 260

        section code,code

; report: a1 = name, d0 = flag (0 = fail).  Clobbers nothing (all saved).
; usage: load a1, put flag in d0, bsr report.

start:
        movea.l 4.w,a6
        move.l  a6,exec_base
        move.l  #dos_name,a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,dos_base
        bne.s   .ok
        rts
.ok:
        move.l  #s_start,a1
        bsr     print

; ---------- T01: Lock("S:", SHARED) ------------------------------------
        movea.l dos_base,a6
        move.l  #s_name_s,d1
        move.l  #SHARED_LOCK,d2
        jsr     LVO_Lock(a6)
        move.l  d0,lock_s
        move.l  #s_t01,a1
        bsr     report

; ---------- T02: Examine(lock, fib) -> dir ------------------------------
        movea.l dos_base,a6
        move.l  lock_s,d1
        move.l  #fib,d2
        jsr     LVO_Examine(a6)
        move.l  d0,d6
        beq.s   .t02f
        move.l  fib+fib_DirEntryType,d0
        tst.l   d0
        sgt     d0
        andi.l  #$FF,d0
        move.l  d0,d6
.t02f:  move.l  d6,d0
        move.l  #s_t02,a1
        bsr     report

; ---------- T03: DupLock + ParentDir ------------------------------------
        movea.l dos_base,a6
        move.l  lock_s,d1
        jsr     LVO_DupLock(a6)
        move.l  d0,d7
        move.l  d0,d6                          ; save dup for later
        move.l  #s_t03a,a1
        bsr     report
        tst.l   d6
        beq.s   .t03_done
        movea.l dos_base,a6
        move.l  d6,d1
        jsr     LVO_ParentDir(a6)
        move.l  d0,d7
        move.l  #s_t03b,a1
        bsr     report
        tst.l   d7
        beq.s   .t03_done
        movea.l dos_base,a6
        move.l  d7,d1
        jsr     LVO_UnLock(a6)
.t03_done:

; ---------- T04: Info(lock, idata) --------------------------------------
        movea.l dos_base,a6
        move.l  lock_s,d1
        move.l  #idata,d2
        jsr     LVO_Info(a6)
        move.l  #s_t04,a1
        bsr     report

; ---------- T05: CurrentDir round-trip ----------------------------------
        movea.l dos_base,a6
        move.l  lock_s,d1
        jsr     LVO_DupLock(a6)
        move.l  d0,d7
        tst.l   d7
        beq.s   .t05_done
        movea.l dos_base,a6
        move.l  d7,d1
        jsr     LVO_CurrentDir(a6)
        move.l  d0,d6                         ; old cwd lock
        movea.l dos_base,a6
        move.l  d6,d1
        jsr     LVO_CurrentDir(a6)            ; restore
        move.l  d0,d1
        beq.s   .t05_done
        movea.l dos_base,a6
        jsr     LVO_UnLock(a6)
.t05_done:
        move.l  d7,d0
        sne     d0
        andi.l  #$FF,d0
        move.l  #s_t05,a1
        bsr     report

; ---------- T06: IsFileSystem("RAM:") -----------------------------------
        movea.l dos_base,a6
        move.l  #s_name_ram,d1
        jsr     LVO_IsFileSystem(a6)
        move.l  #s_t06,a1
        bsr     report

; ---------- T07: DeviceProc("S:") ---------------------------------------
        movea.l dos_base,a6
        move.l  #s_name_s,d1
        jsr     LVO_DeviceProc(a6)
        move.l  #s_t07,a1
        bsr     report

; ---------- T08: create+write+SetFileSize+read round-trip via S: --------
        movea.l dos_base,a6
        move.l  #s_name_tmp,d1
        move.l  #MODE_NEWFILE,d2
        jsr     LVO_Open(a6)
        move.l  d0,fh
        move.l  #s_t08a,a1
        bsr     report
        tst.l   fh
        beq     .t08_done

        movea.l dos_base,a6
        move.l  fh,d1
        move.l  #pattern,d2
        move.l  #16,d3
        jsr     LVO_Write(a6)
        cmp.l   #16,d0
        seq     d0
        andi.l  #$FF,d0
        move.l  #s_t08b,a1
        bsr     report

        movea.l dos_base,a6
        move.l  fh,d1
        move.l  #64,d2
        move.l  #OFFSET_BEGINNING,d3
        jsr     LVO_SetFileSize(a6)
        cmp.l   #64,d0
        seq     d0
        andi.l  #$FF,d0
        move.l  #s_t08c,a1
        bsr     report

        movea.l dos_base,a6
        move.l  fh,d1
        moveq   #0,d2
        move.l  #OFFSET_BEGINNING,d3
        jsr     LVO_Seek(a6)
        movea.l dos_base,a6
        move.l  fh,d1
        move.l  #buf,d2
        move.l  #64,d3
        jsr     LVO_Read(a6)
        cmp.l   #64,d0
        bne.s   .t08d_fail
        lea     buf,a0
        lea     pattern,a1
        moveq   #15,d0
.cmp1:  move.b  (a0)+,d1
        cmp.b   (a1)+,d1
        bne.s   .t08d_fail
        dbra    d0,.cmp1
        moveq   #47,d0
.cmp2:  tst.b   (a0)+
        bne.s   .t08d_fail
        moveq   #1,d0
        bra.s   .t08d_r
.t08d_fail:
        moveq   #0,d0
.t08d_r:
        move.l  #s_t08d,a1
        bsr     report

; ---------- T09: ExamineFH -> fib_Size == 64 -----------------------------
        movea.l dos_base,a6
        move.l  fh,d1
        move.l  #fib,d2
        jsr     LVO_ExamineFH(a6)
        move.l  d0,d6
        beq.s   .t09f
        move.l  fib+fib_Size,d0
        cmp.l   #64,d0
        seq     d6
        andi.l  #$FF,d6
.t09f:  move.l  d6,d0
        move.l  #s_t09,a1
        bsr     report

; ---------- T10: DupLockFromFH + ParentOfFH -----------------------------
        movea.l dos_base,a6
        move.l  fh,d1
        jsr     LVO_DupLockFromFH(a6)
        move.l  d0,d7
        move.l  #s_t10a,a1
        bsr     report
        tst.l   d7
        beq.s   .t10b
        movea.l dos_base,a6
        move.l  d7,d1
        jsr     LVO_UnLock(a6)
.t10b:
        movea.l dos_base,a6
        move.l  fh,d1
        jsr     LVO_ParentOfFH(a6)
        move.l  d0,d7
        move.l  #s_t10b,a1
        bsr     report
        tst.l   d7
        beq.s   .t10c
        movea.l dos_base,a6
        move.l  d7,d1
        jsr     LVO_UnLock(a6)
.t10c:

        movea.l dos_base,a6
        move.l  fh,d1
        jsr     LVO_Close(a6)
        move.l  #0,fh
.t08_done:

; ---------- T11: SetComment/SetProtection/Rename/DeleteFile via S: ------
        movea.l dos_base,a6
        move.l  #s_name_tmp,d1
        move.l  #s_comment,d2
        jsr     LVO_SetComment(a6)
        move.l  #s_t11a,a1
        bsr     report

        movea.l dos_base,a6
        move.l  #s_name_tmp,d1
        moveq   #0,d2
        jsr     LVO_SetProtect(a6)
        move.l  #s_t11b,a1
        bsr     report

        movea.l dos_base,a6
        move.l  #s_name_tmp,d1
        move.l  #s_name_tmp2,d2
        jsr     LVO_Rename(a6)
        move.l  #s_t11c,a1
        bsr     report

        movea.l dos_base,a6
        move.l  #s_name_tmp2,d1
        jsr     LVO_DeleteFile(a6)
        move.l  #s_t11d,a1
        bsr     report

; ---------- T12: CreateDir + DeleteFile on RAM: -------------------------
        movea.l dos_base,a6
        move.l  #s_name_dir,d1
        jsr     LVO_CreateDir(a6)
        move.l  d0,d7
        move.l  #s_t12a,a1
        bsr     report
        tst.l   d7
        beq.s   .t12_done
        movea.l dos_base,a6
        move.l  d7,d1
        jsr     LVO_UnLock(a6)
        movea.l dos_base,a6
        move.l  #s_name_dir,d1
        jsr     LVO_DeleteFile(a6)
        move.l  #s_t12b,a1
        bsr     report
.t12_done:

; ---------- T13: ExAll/ExAllEnd over S: ---------------------------------
        move.l  #0,eac                      ; eac_Entries = 0
        move.l  #0,eac+4                    ; eac_LastKey
        movea.l dos_base,a6
        move.l  lock_s,d1
        move.l  #exbuf,d2
        move.l  #2048,d3
        move.l  #2,d4                        ; ED_TYPE
        move.l  #eac,d5
        jsr     LVO_ExAll(a6)
        move.l  eac,d0                        ; eac_Entries > 0?
        sne     d0
        andi.l  #$FF,d0
        move.l  #s_t13,a1
        bsr     report
        movea.l dos_base,a6
        move.l  lock_s,d1
        move.l  #exbuf,d2
        move.l  #2048,d3
        move.l  #2,d4
        move.l  #eac,d5
        jsr     LVO_ExAllEnd(a6)

; ---------- T14: OpenFromLock -------------------------------------------
        movea.l dos_base,a6
        move.l  #s_name_tmp3,d1
        move.l  #MODE_NEWFILE,d2
        jsr     LVO_Open(a6)
        move.l  d0,d6
        beq.s   .t14_fail
        movea.l dos_base,a6
        move.l  d6,d1
        jsr     LVO_Close(a6)
        movea.l dos_base,a6
        move.l  #s_name_tmp3,d1
        move.l  #SHARED_LOCK,d2
        jsr     LVO_Lock(a6)
        move.l  d0,d7
        beq.s   .t14_fail
        movea.l dos_base,a6
        move.l  d7,d1
        jsr     LVO_OpenFromLock(a6)          ; consumes lock
        move.l  d0,d6
        beq.s   .t14_fail
        movea.l dos_base,a6
        move.l  d6,d1
        jsr     LVO_Close(a6)
        moveq   #1,d0
        bra.s   .t14_r
.t14_fail:
        moveq   #0,d0
.t14_r: move.l  #s_t14,a1
        bsr     report
        movea.l dos_base,a6
        move.l  #s_name_tmp3,d1
        jsr     LVO_DeleteFile(a6)

; ---------- T15: SameLock(lock_s, dup) ----------------------------------
        movea.l dos_base,a6
        move.l  lock_s,d1
        jsr     LVO_DupLock(a6)
        move.l  d0,d7
        moveq   #0,d6
        tst.l   d7
        beq.s   .t15_r
        movea.l dos_base,a6
        move.l  lock_s,d1
        move.l  d7,d2
        jsr     LVO_SameLock(a6)
        move.l  d0,d6
        movea.l dos_base,a6
        move.l  d7,d1
        jsr     LVO_UnLock(a6)
.t15_r: move.l  d6,d0
        move.l  #s_t15,a1
        bsr     report

; ---------- T16: AssignAdd("T248:", lock on RAM:) then Lock -------------
        movea.l dos_base,a6
        move.l  #s_name_ram,d1
        move.l  #SHARED_LOCK,d2
        jsr     LVO_Lock(a6)
        move.l  d0,d7
        moveq   #0,d6
        tst.l   d7
        beq.s   .t16_r
        movea.l dos_base,a6
        move.l  #s_name_t248,d1
        move.l  d7,d2
        jsr     LVO_AssignAdd(a6)             ; consumes lock on success
        move.l  d0,d6
        beq.s   .t16_r
        movea.l dos_base,a6
        move.l  #s_name_t248dir,d1
        move.l  #SHARED_LOCK,d2
        jsr     LVO_Lock(a6)
        move.l  d0,d6
        beq.s   .t16_r
        movea.l dos_base,a6
        move.l  d6,d1
        jsr     LVO_UnLock(a6)
        moveq   #1,d6
.t16_r: move.l  d6,d0
        move.l  #s_t16,a1
        bsr     report

; ---------- T17: Inhibit("RAM:",1)/un-inhibit ----------------------------
        movea.l dos_base,a6
        move.l  #s_name_ram,d1
        move.l  #1,d2
        jsr     LVO_Inhibit(a6)
        move.l  d0,d7
        movea.l dos_base,a6
        move.l  #s_name_ram,d1
        moveq   #0,d2
        jsr     LVO_Inhibit(a6)               ; un-inhibit regardless
        move.l  d7,d0
        move.l  #s_t17,a1
        bsr     report

; ---------- T18: guest Open on OCTAMED: instrument (LFN path) -----------
        movea.l dos_base,a6
        move.l  #s_name_oct,d1
        move.l  #SHARED_LOCK,d2
        jsr     LVO_Lock(a6)                  ; skip if OCTAMED: absent
        tst.l   d0
        beq     .t18_skip
        move.l  d0,d1
        movea.l dos_base,a6
        jsr     LVO_UnLock(a6)
        movea.l dos_base,a6
        move.l  #s_name_instr,d1
        move.l  #MODE_OLDFILE,d2
        jsr     LVO_Open(a6)
        move.l  d0,d7
        beq.s   .t18_r
        move.l  d0,d1
        movea.l dos_base,a6
        jsr     LVO_Close(a6)
        moveq   #1,d7
.t18_r: move.l  d7,d0
        move.l  #s_t18,a1
        bsr     report
        bra.s   .t18_done
.t18_skip:
        move.l  #s_t18s,a1
        bsr     print
.t18_done:

        movea.l dos_base,a6
        move.l  lock_s,d1
        jsr     LVO_UnLock(a6)

        move.l  #s_done,a1
        bsr     print
        movea.l exec_base,a6
        move.l  dos_base,a1
        jsr     LVO_CloseLibrary(a6)
        rts

; ---------------------------------------------------------------------
; helpers
; ---------------------------------------------------------------------
print:                                     ; a1 = C string
        movem.l d0-d7/a0-a6,-(sp)
        movea.l dos_base,a6
        move.l  a1,d1
        jsr     LVO_PutStr(a6)
        movem.l (sp)+,d0-d7/a0-a6
        rts

report:                                    ; a1 = name, d0 = flag
        movem.l d0-d7/a0-a6,-(sp)
        bsr     print
        move.l  (sp),d0                      ; saved d0 from stack
        tst.l   d0
        beq.s   .f
        move.l  #s_pass,a1
        bsr     print
        bra.s   .r
.f:     move.l  #s_fail,a1
        bsr     print
        movea.l dos_base,a6
        jsr     LVO_IoErr(a6)
        move.l  d0,d7
        move.l  #s_ioerr,a1
        bsr     print
        move.l  d7,d0
        bsr     print_dec
        move.l  #s_nl,a1
        bsr     print
.r:     movem.l (sp)+,d0-d7/a0-a6
        rts

print_dec:                                 ; d0 = unsigned value
        movem.l d0-d7/a0-a6,-(sp)
        move.l  #numbuf+11,a0
        move.b  #0,(a0)
.pd:    divu    #10,d0
        swap    d0
        add.b   #'0',d0
        move.b  d0,-(a0)
        clr.w   d0
        swap    d0
        tst.l   d0
        bne.s   .pd
        move.l  a0,a1
        bsr     print
        movem.l (sp)+,d0-d7/a0-a6
        rts

        section bss,bss
exec_base:  ds.l 1
dos_base:   ds.l 1
lock_s:     ds.l 1
fh:         ds.l 1
fib:        ds.b FIB_SIZE
idata:      ds.b 36
buf:        ds.b 80
exbuf:      ds.b 2048
eac:        ds.b 20
numbuf:     ds.b 12

        section data,data
dos_name:      dc.b "dos.library",0
s_name_s:      dc.b "S:",0
s_name_ram:    dc.b "RAM:",0
s_name_oct:    dc.b "OCTAMED:",0
s_name_tmp:    dc.b "S:u248test.tmp",0
s_name_tmp2:   dc.b "S:u248test2.tmp",0
s_name_tmp3:   dc.b "RAM:u248t3.tmp",0
s_name_dir:    dc.b "RAM:u248dir",0
s_name_t248:   dc.b "T248:",0
s_name_t248dir: dc.b "T248:",0
s_name_instr:  dc.b "OCTAMED:INSTRUMENTS/HighHat.instr",0
s_comment:     dc.b "uaos248",0
pattern:       dc.b "0123456789ABCDEF"
s_start:       dc.b "DOSTEST START",10,0
s_done:        dc.b "DOSTEST DONE",10,0
s_pass:        dc.b " PASS",10,0
s_fail:        dc.b " FAIL",0
s_ioerr:       dc.b " ioerr=",0
s_nl:          dc.b 10,0
s_t01:  dc.b "T01 lock S:",0
s_t02:  dc.b "T02 examine S:",0
s_t03a: dc.b "T03a duplock",0
s_t03b: dc.b "T03b parentdir",0
s_t04:  dc.b "T04 info",0
s_t05:  dc.b "T05 currentdir",0
s_t06:  dc.b "T06 isfilesystem",0
s_t07:  dc.b "T07 deviceproc",0
s_t08a: dc.b "T08a create S:",0
s_t08b: dc.b "T08b write",0
s_t08c: dc.b "T08c setfilesize",0
s_t08d: dc.b "T08d readback",0
s_t09:  dc.b "T09 examinefh",0
s_t10a: dc.b "T10a duplockfromfh",0
s_t10b: dc.b "T10b parentoffh",0
s_t11a: dc.b "T11a setcomment",0
s_t11b: dc.b "T11b setprotection",0
s_t11c: dc.b "T11c rename",0
s_t11d: dc.b "T11d deletefile",0
s_t12a: dc.b "T12a createdir",0
s_t12b: dc.b "T12b deletedir",0
s_t13:  dc.b "T13 exall",0
s_t14:  dc.b "T14 openfromlock",0
s_t15:  dc.b "T15 samelock",0
s_t16:  dc.b "T16 assignadd",0
s_t17:  dc.b "T17 inhibit",0
s_t18:  dc.b "T18 octamed instr open",0
s_t18s: dc.b "T18 skipped (no OCTAMED:)",10,0
        even
        end
