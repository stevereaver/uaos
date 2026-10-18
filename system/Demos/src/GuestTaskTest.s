; GuestTaskTest.s — UAOS-307 acceptance probe.
;
; A guest calls exec AddTask() for a worker routine living in the same
; guest RAM window.  The worker bumps a flag, PutMsg()es the parent port
; and Signal()s the parent task — proving the second M68k context is
; scheduled, can run exec calls, and hands messages/signals back.
;
;   GUESTTASK PASS  -> worker ran, message arrived, signal delivered
;   GUESTTASK FAIL  -> added task never ran (the known pre-UAOS-307 gap:
;                      AddTask only links the node on TaskWait)
;
; Assemble: vasmm68k_mot -Fhunk -o GuestTaskTest.o GuestTaskTest.s
; Link:     vlink -bamigahunk -o GuestTaskTest.hunk GuestTaskTest.o
;

LVO_OpenLibrary  equ -552
LVO_CloseLibrary equ -414
LVO_PutStr       equ -948
LVO_Delay        equ -198

LV_AddTask       equ -282
LV_RemTask       equ -288
LV_FindTask      equ -294
LV_Wait          equ -318
LV_Signal        equ -324
LV_AllocSignal   equ -330
LV_PutMsg        equ -366
LV_GetMsg        equ -372
LV_RawPutChar    equ -516
LV_CreateMsgPort equ -666
LV_DeleteMsgPort equ -672
LV_SetSignal     equ -306

; Task / Process / Message layout
LN_TYPE     equ 8
LN_PRI      equ 9
LN_NAME     equ 10
NT_TASK     equ 1
NT_MESSAGE  equ 5
TC_SIGRECVD equ 26
TC_SPREG    equ 54
TC_SPLOWER  equ 58
TC_SPUPPER  equ 62
TC_IDNEST   equ 16
TC_TDNEST   equ 17
MN_REPLYPORT equ 14
MN_LENGTH   equ 20

TASK_SIZE   equ 96
STACK_SIZE  equ 1024

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

; ---- reply port + a signal bit ---------------------------------------
        movea.l exec_base,a6
        jsr     LV_CreateMsgPort(a6)
        move.l  d0,msg_port
        bne.s   .port_ok
        lea     s_fport(pc),a1
        bra     do_fail
.port_ok:
        moveq   #-1,d0
        jsr     LV_AllocSignal(a6)
        move.l  d0,sig_num
        bpl.s   .sig_ok
        lea     s_fsig(pc),a1
        bra     do_fail
.sig_ok:
        move.l  sig_num,d1
        moveq   #1,d2
        lsl.l   d1,d2
        move.l  d2,sig_mask
        ; clear it in case it is already pending
        moveq   #0,d0
        move.l  d2,d1
        jsr     LV_SetSignal(a6)
        ; remember our own task address for the worker to Signal()
        suba.l  a1,a1
        jsr     LV_FindTask(a6)
        move.l  d0,parent_tc

; ---- build a Task node for the worker --------------------------------
        lea     worker_task,a1
        move.b  #NT_TASK,LN_TYPE(a1)
        move.b  #0,LN_PRI(a1)
        lea     wt_name(pc),a0
        move.l  a0,LN_NAME(a1)
        move.b  #-1,TC_IDNEST(a1)
        move.b  #-1,TC_TDNEST(a1)
        clr.l   18(a1)                  ; tc_SigAlloc
        clr.l   22(a1)                  ; tc_SigWait
        clr.l   26(a1)                  ; tc_SigRecvd
        clr.l   30(a1)                  ; tc_SigExcept
        lea     worker_stack,a0
        move.l  a0,TC_SPLOWER(a1)
        lea     worker_stack+STACK_SIZE-16,a0
        move.l  a0,TC_SPUPPER(a1)
        move.l  a0,TC_SPREG(a1)

; ---- AddTask(worker_task, worker, NULL) -------------------------------
        lea     worker_task,a1
        lea     worker(pc),a2
        suba.l  a3,a3
        jsr     LV_AddTask(a6)
        lea     worker_task,a0
        cmp.l   a0,d0
        beq.s   .at_ok
        lea     s_faddtask(pc),a1
        bra     do_fail
.at_ok:
        lea     s_added(pc),a1
        bsr     print

; ---- poll up to ~10 s: worker_flag, reply message, signal bit --------
        moveq   #20,d7
.poll:
        movea.l dos_base,a6
        moveq   #10,d1                  ; 0.5 s
        move.l  d1,d0                   ; legacy UAOS build read d0 — belt+braces
        jsr     LVO_Delay(a6)
        tst.l   worker_flag
        bne.s   .ran
        subq.l  #1,d7
        bne.s   .poll

        ; never ran — the documented UAOS-307 gap
        lea     s_never(pc),a1
        bra     do_fail

.ran:
        lea     s_ran(pc),a1
        bsr     print

        ; PutMsg() from the worker must have queued a node on the port
        movea.l exec_base,a6
        movea.l msg_port,a0
        jsr     LV_GetMsg(a6)
        tst.l   d0
        bne.s   .msg_ok
        lea     s_nomsg(pc),a1
        bra     do_fail
.msg_ok:
        lea     s_gotmsg(pc),a1
        bsr     print

        ; Signal() from the worker must have set our sig bit
        movea.l parent_tc,a0
        move.l  TC_SIGRECVD(a0),d0
        and.l   sig_mask,d0
        bne.s   .sig_got
        lea     s_nosig(pc),a1
        bra     do_fail
.sig_got:
        lea     s_gotsig(pc),a1
        bsr     print

        lea     s_pass(pc),a1
        bsr     print
        bra     out

do_fail:
        bsr     print
        lea     s_fail(pc),a1
        bsr     print
out:
        movea.l exec_base,a6
        movea.l msg_port,a0
        jsr     LV_DeleteMsgPort(a6)
        movea.l dos_base,a1
        jsr     LVO_CloseLibrary(a6)
early_exit:
        rts

; -----------------------------------------------------------------------
; worker — entered on initPC by AddTask.  Runs in the same window.
; -----------------------------------------------------------------------
worker:
        movea.l 4.w,a6
        addq.l  #1,worker_flag
        ; hand the parent a message on its reply port
        lea     worker_msg,a1
        move.b  #NT_MESSAGE,LN_TYPE(a1)
        move.l  #32,MN_LENGTH(a1)
        clr.l   MN_REPLYPORT(a1)
        movea.l msg_port,a0
        jsr     LV_PutMsg(a6)
        ; and signal it
        movea.l parent_tc,a1
        move.l  sig_mask,d0
        jsr     LV_Signal(a6)
        ; self-remove / exit context
        suba.l  a1,a1
        jsr     LV_RemTask(a6)
        rts                             ; finalPC fallback

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

        ; single-section layout: PC-relative refs resolve at assembly time
        section code,code

dos_name:   dc.b "dos.library",0
        even
wt_name:    dc.b "GTWorker",0
        even
s_start:    dc.b "GUESTTASKTEST (UAOS-307)",10,0
        even
s_added:    dc.b "addtask: worker node linked",10,0
        even
s_ran:      dc.b "worker ran",10,0
        even
s_gotmsg:   dc.b "worker message arrived on reply port",10,0
        even
s_gotsig:   dc.b "worker signal delivered",10,0
        even
s_never:    dc.b "added task never ran",10,0
        even
s_nomsg:    dc.b "worker ran but no message arrived",10,0
        even
s_nosig:    dc.b "worker ran+msg but signal not delivered",10,0
        even
s_fport:    dc.b "FAIL CreateMsgPort",10,0
        even
s_fsig:     dc.b "FAIL AllocSignal",10,0
        even
s_faddtask: dc.b "FAIL AddTask ret",10,0
        even
s_pass:     dc.b "GUESTTASK PASS",10,0
        even
s_fail:     dc.b "GUESTTASK FAIL",10,0
        even

exec_base:   ds.l 1
dos_base:    ds.l 1
msg_port:    ds.l 1
parent_tc:   ds.l 1
sig_num:     ds.l 1
sig_mask:    ds.l 1
worker_flag: ds.l 1
worker_msg:  ds.b 32
        even
worker_task: ds.b TASK_SIZE
worker_stack: ds.b STACK_SIZE

        end
