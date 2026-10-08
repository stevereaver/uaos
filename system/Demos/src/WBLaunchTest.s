; WBLaunchTest.s — UAOS-253 Workbench launch semantics acceptance test.
;
; Verifies the full WB launch contract:
;   1. pr_CLI == 0           (launched from Workbench, not CLI)
;   2. WBStartup message queued on pr_MsgPort (WaitPort/GetMsg)
;   3. sm_NumArgs >= 1, sm_Segment != 0, sm_ArgList valid
;   4. WBArg[0].wa_Name printed, wa_Lock becomes CurrentDir
;   5. icon.library: GetDiskObject/FindToolType/MatchToolValue
;   6. PutDiskObject("RAM:WBPUT") round-trips to RAM:WBPUT.info
;   7. FreeDiskObject, ReplyMsg(WBStartup)
; Prints "WBLAUNCH PASS" on stdout on success.

LVO_FindTask     equ -294
LVO_GetMsg       equ -372
LVO_ReplyMsg     equ -378
LVO_WaitPort     equ -384
LVO_OpenLibrary  equ -552
LVO_CloseLibrary equ -414

LVO_Open         equ -30
LVO_Close        equ -36
LVO_PutStr       equ -948
LVO_CurrentDir   equ -126

ICON_GetDiskObject  equ -78
ICON_PutDiskObject  equ -84
ICON_FreeDiskObject equ -90
ICON_FindToolType   equ -96
ICON_MatchToolValue equ -102

MODE_OLDFILE equ 1005

; struct Process fields
PR_MSGPORT   equ $5C
PR_CLI       equ $AC

; struct WBStartup payload (Message = 20 bytes)
SM_SEGMENT   equ $18
SM_NUMARGS   equ $1C
SM_ARGLIST   equ $24

; struct DiskObject
DO_TOOLTYPES equ $36

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

        ; ---- 1. Must be a Workbench launch: pr_CLI == 0 ----
        movea.l exec_base,a6
        suba.l  a1,a1
        jsr     LVO_FindTask(a6)
        move.l  d0,a4                   ; a4 = struct Process
        tst.l   d0
        bne.s   .proc_ok
        lea     s_fproc(pc),a1
        bra     do_fail
.proc_ok:
        tst.l   PR_CLI(a4)
        beq.s   .is_wb
        lea     s_fcli(pc),a1
        bra     do_fail
.is_wb:
        lea     s_prcli(pc),a1
        bsr     print

        ; ---- 2. Fetch the WBStartup message ----
        lea     PR_MSGPORT(a4),a0
        jsr     LVO_WaitPort(a6)
        lea     PR_MSGPORT(a4),a0
        jsr     LVO_GetMsg(a6)
        move.l  d0,a5                   ; a5 = struct WBStartup
        bne.s   .got_msg
        lea     s_fmsg(pc),a1
        bra     do_fail
.got_msg:
        lea     s_gotmsg(pc),a1
        bsr     print

        ; ---- 3. Validate fields ----
        move.l  SM_NUMARGS(a5),d0
        move.l  d0,-(sp)
        lea     s_nargs(pc),a1
        bsr     print
        move.l  (sp)+,d0
        bsr     print_dec
        cmpi.l  #1,d0
        bge.s   .na_ok
        lea     s_fnargs(pc),a1
        bra     do_fail_reply
.na_ok:
        tst.l   SM_SEGMENT(a5)
        bne.s   .seg_ok
        lea     s_fseg(pc),a1
        bra     do_fail_reply
.seg_ok:
        move.l  SM_ARGLIST(a5),a3       ; a3 = WBArg[]
        cmpa.l  #0,a3
        bne.s   .args_ok
        lea     s_fargs(pc),a1
        bra     do_fail_reply
.args_ok:

        ; ---- 4. WBArg[0] = the tool itself ----
        lea     s_arg0(pc),a1
        bsr     print
        movea.l 4(a3),a1                ; wa_Name
        bsr     print
        lea     s_nl(pc),a1
        bsr     print

        ; CurrentDir(arg0 lock) — tools resolve their own files via it
        movea.l dos_base,a6
        move.l  0(a3),d1                ; wa_Lock
        beq.s   .no_lock
        jsr     LVO_CurrentDir(a6)
.no_lock:

        ; ---- 5. icon.library ----
        movea.l exec_base,a6
        lea     icon_name(pc),a1
        moveq   #36,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,icon_base
        bne.s   .icon_ok
        lea     s_ficon(pc),a1
        bra     do_fail_reply
.icon_ok:

        ; GetDiskObject(wa_Name) — resolves "<name>.info" via cwd
        movea.l icon_base,a6
        movea.l 4(a3),a0
        jsr     ICON_GetDiskObject(a6)
        move.l  d0,dobj
        bne.s   .dobj_ok
        lea     s_fdobj(pc),a1
        bra     do_fail_reply
.dobj_ok:
        lea     s_dobj(pc),a1
        bsr     print

        ; FindToolType(dobj->do_ToolTypes, "TESTKEY")
        movea.l dobj,a0
        movea.l DO_TOOLTYPES(a0),a0
        cmpa.l  #0,a0
        bne.s   .ttarr_ok
        lea     s_fttarr(pc),a1
        bra     do_fail_reply
.ttarr_ok:
        lea     tt_testkey(pc),a1
        movea.l icon_base,a6
        jsr     ICON_FindToolType(a6)
        move.l  d0,d5
        bne.s   .tt_ok
        lea     s_ftt(pc),a1
        bra     do_fail_reply
.tt_ok:
        lea     s_tt(pc),a1
        bsr     print
        movea.l d5,a1
        bsr     print
        lea     s_nl(pc),a1
        bsr     print

        ; MatchToolValue(tt, "VALUE42") must be TRUE
        movea.l d5,a0
        lea     tt_val42(pc),a1
        movea.l icon_base,a6
        jsr     ICON_MatchToolValue(a6)
        tst.l   d0
        bne.s   .mtv_ok
        lea     s_fmtv(pc),a1
        bra     do_fail_reply
.mtv_ok:

        ; ---- 6. PutDiskObject round-trip ----
        lea     put_name(pc),a0
        movea.l dobj,a1
        movea.l icon_base,a6
        jsr     ICON_PutDiskObject(a6)
        tst.l   d0
        bne.s   .put_ok
        lea     s_fput(pc),a1
        bra     do_fail_reply
.put_ok:
        movea.l dos_base,a6
        lea     put_info(pc),a1
        move.l  a1,d1
        move.l  #MODE_OLDFILE,d2
        jsr     LVO_Open(a6)
        move.l  d0,fh
        bne.s   .open_ok
        lea     s_fopen(pc),a1
        bra     do_fail_reply
.open_ok:
        move.l  fh,d1
        jsr     LVO_Close(a6)
        lea     s_putok(pc),a1
        bsr     print

        ; ---- 7. Cleanup: FreeDiskObject + ReplyMsg ----
        movea.l dobj,a0
        movea.l icon_base,a6
        jsr     ICON_FreeDiskObject(a6)

        movea.l a5,a1
        movea.l exec_base,a6
        jsr     LVO_ReplyMsg(a6)
        lea     s_replied(pc),a1
        bsr     print

        lea     s_pass(pc),a1
        bsr     print
        bra.s   done

; ---- failure exits ----------------------------------------------------
; do_fail       : print a1, exit (WBStartup may not be held)
; do_fail_reply : print a1, ReplyMsg if we hold it, exit
do_fail:
        bsr     print
        bra.s   done

do_fail_reply:
        bsr     print
        cmpa.l  #0,a5
        beq.s   done
        movea.l a5,a1
        movea.l exec_base,a6
        jsr     LVO_ReplyMsg(a6)
        bra.s   done

done:
        rts

; ---- helpers ----------------------------------------------------------
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
        divu    #10,d0
        swap    d0
        add.b   #'0',d0
        move.b  d0,-(a0)
        clr.w   d0
        swap    d0
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
icon_base:  dc.l 0
dobj:       dc.l 0
fh:         dc.l 0

dos_name:   dc.b "dos.library",0
icon_name:  dc.b "icon.library",0
tt_testkey: dc.b "TESTKEY",0
tt_val42:   dc.b "VALUE42",0
put_name:   dc.b "RAM:WBPUT",0
put_info:   dc.b "RAM:WBPUT.info",0

s_start:    dc.b "WBLAUNCH begin",10,0
s_prcli:    dc.b "pr_CLI = 0 (Workbench launch)",10,0
s_gotmsg:   dc.b "WBStartup message received",10,0
s_nargs:    dc.b "sm_NumArgs = ",0
s_nl:       dc.b "'",10,0
s_arg0:     dc.b "arg0 name='",0
s_dobj:     dc.b "GetDiskObject ok",10,0
s_tt:       dc.b "tooltype 'TESTKEY' = '",0
s_putok:    dc.b "PutDiskObject wrote RAM:WBPUT.info",10,0
s_replied:  dc.b "WBStartup replied",10,0
s_pass:     dc.b "WBLAUNCH PASS",10,0

s_fproc:    dc.b "WBLAUNCH FAIL FindTask",10,0
s_fcli:     dc.b "WBLAUNCH FAIL pr_CLI != 0 (CLI launch)",10,0
s_fmsg:     dc.b "WBLAUNCH FAIL no WBStartup message",10,0
s_fnargs:   dc.b "WBLAUNCH FAIL sm_NumArgs < 1",10,0
s_fseg:     dc.b "WBLAUNCH FAIL sm_Segment = 0",10,0
s_fargs:    dc.b "WBLAUNCH FAIL sm_ArgList = 0",10,0
s_ficon:    dc.b "WBLAUNCH FAIL OpenLibrary(icon)",10,0
s_fdobj:    dc.b "WBLAUNCH FAIL GetDiskObject",10,0
s_fttarr:   dc.b "WBLAUNCH FAIL do_ToolTypes = 0",10,0
s_ftt:      dc.b "WBLAUNCH FAIL FindToolType(TESTKEY)",10,0
s_fmtv:     dc.b "WBLAUNCH FAIL MatchToolValue(VALUE42)",10,0
s_fput:     dc.b "WBLAUNCH FAIL PutDiskObject",10,0
s_fopen:    dc.b "WBLAUNCH FAIL RAM:WBPUT.info missing",10,0

numbuf:     ds.b 16

        end
