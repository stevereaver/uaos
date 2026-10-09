; DevIOTest.s — UAOS-240 acceptance: guest device I/O framework.
;
; Exercises the OpenDevice + IORequest completion model:
;   * CreateMsgPort / CreateIORequest (exec glue)
;   * timer.device:  SendIO(TR_ADDREQUEST) must pend (CheckIO == 0),
;     the expiry reply lands on mn_ReplyPort as a message and signals
;     mp_SigTask/mp_SigBit — Wait()+GetMsg() must return the ioreq.
;     DoIO(TR_GETSYSTIME) fills tv_secs.
;   * console.device: opens, CMD_WRITE emits through the print hook.
;   * serial.device:  OpenDevice must fail (declined device policy).
;   * keyboard.device: opens; SendIO(KBD_READEVENT) pends; AbortIO +
;     WaitIO return IOERR_ABORTED — the pending-read completion path.
;
; Prints "DEVIOTEST PASS" or "DEVIOTEST FAIL <stage>" on stdout/serial.

LVO_OpenLibrary  equ -552
LVO_PutStr       equ -948
LVO_OpenDevice   equ -444
LVO_CloseDevice  equ -450
LVO_DoIO         equ -456
LVO_SendIO       equ -462
LVO_CheckIO      equ -468
LVO_WaitIO       equ -474
LVO_AbortIO      equ -480
LV_FindTask      equ -294
LV_Wait          equ -318
LV_AllocSignal   equ -330
LV_FreeSignal    equ -336
LV_GetMsg        equ -372
LV_RawPutChar    equ -516
LV_CreateIOReq   equ -654
LV_DeleteIOReq   equ -660
LV_CreateMsgPort equ -666
LV_DeleteMsgPort equ -672

; struct IORequest / IOStdReq
MN_REPLYPORT equ 14
IO_DEVICE    equ 20
IO_COMMAND   equ 28
IO_ERROR     equ 31
IO_ACTUAL    equ 32
IO_LENGTH    equ 36
IO_DATA      equ 40
IOSTD_SIZE   equ 48

; MsgPort
MP_SIGBIT    equ 15
MP_SIGTASK   equ 16

; timerequest extension
TR_SECS      equ 32
TR_MICRO     equ 36

; commands
CMD_WRITE       equ 3
TR_ADDREQUEST   equ 9
TR_GETSYSTIME   equ 10
KBD_READEVENT   equ 12

IOERR_ABORTED   equ -2
IE_SIZE         equ 22

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

; ---- reply port ------------------------------------------------------
        movea.l exec_base,a6
        jsr     LV_CreateMsgPort(a6)
        move.l  d0,reply_port
        bne.s   .port_ok
        lea     s_fport(pc),a1
        bra     do_fail
.port_ok:

; ---- timer.device ioreq ----------------------------------------------
        movea.l reply_port,a0
        moveq   #IOSTD_SIZE,d0
        jsr     LV_CreateIOReq(a6)
        move.l  d0,timer_io
        bne.s   .tio_ok
        lea     s_fio(pc),a1
        bra     do_fail
.tio_ok:

        lea     timer_name(pc),a0
        moveq   #0,d0                   ; UNIT_MICROHZ
        movea.l timer_io,a1
        moveq   #0,d1
        jsr     LVO_OpenDevice(a6)
        tst.l   d0
        beq.s   .top_ok
        lea     s_fopen(pc),a1
        bra     do_fail
.top_ok:

; ---- SendIO(TR_ADDREQUEST, 0.2s) must pend ---------------------------
        movea.l timer_io,a1
        move.w  #TR_ADDREQUEST,IO_COMMAND(a1)
        clr.l   TR_SECS(a1)
        move.l  #200000,TR_MICRO(a1)
        jsr     LVO_SendIO(a6)

        ; while the device holds it, CheckIO must return 0
        movea.l timer_io,a1
        jsr     LVO_CheckIO(a6)
        tst.l   d0
        beq.s   .chk_ok
        lea     s_fchk(pc),a1
        bra     do_fail
.chk_ok:

        ; expiry reply signals mp_SigBit on mp_SigTask
        movea.l reply_port,a0
        moveq   #0,d0
        move.b  MP_SIGBIT(a0),d0
        moveq   #1,d1
        lsl.l   d0,d1
        move.l  d1,d0
        jsr     LV_Wait(a6)

        ; the request node must arrive on the reply port
        movea.l reply_port,a0
        jsr     LV_GetMsg(a6)
        cmp.l   timer_io,d0
        beq.s   .got_reply
        lea     s_freply(pc),a1
        bra     do_fail
.got_reply:

        movea.l timer_io,a1
        jsr     LVO_CheckIO(a6)
        cmp.l   timer_io,d0
        beq.s   .chk2_ok
        lea     s_fchk2(pc),a1
        bra     do_fail
.chk2_ok:
        movea.l timer_io,a1
        tst.b   IO_ERROR(a1)
        beq.s   .terr_ok
        lea     s_ferr(pc),a1
        bra     do_fail
.terr_ok:

; ---- DoIO(TR_GETSYSTIME) ---------------------------------------------
        movea.l timer_io,a1
        move.w  #TR_GETSYSTIME,IO_COMMAND(a1)
        jsr     LVO_DoIO(a6)
        tst.b   d0
        beq.s   .sys_ok
        lea     s_fsys(pc),a1
        bra     do_fail
.sys_ok:
        movea.l timer_io,a1
        tst.l   TR_SECS(a1)             ; epoch must be nonzero
        bne.s   .secs_ok
        lea     s_fsecs(pc),a1
        bra     do_fail
.secs_ok:
        lea     s_tmr(pc),a1
        bsr     print

        movea.l timer_io,a1
        movea.l exec_base,a6
        jsr     LVO_CloseDevice(a6)

; ---- console.device open + CMD_WRITE ---------------------------------
        movea.l reply_port,a0
        moveq   #IOSTD_SIZE,d0
        jsr     LV_CreateIOReq(a6)
        move.l  d0,con_io
        bne.s   .cio_ok
        lea     s_fio(pc),a1
        bra     do_fail
.cio_ok:
        lea     con_name(pc),a0
        moveq   #0,d0
        movea.l con_io,a1
        moveq   #0,d1
        jsr     LVO_OpenDevice(a6)
        tst.l   d0
        beq.s   .cop_ok
        lea     s_fcon(pc),a1
        bra     do_fail
.cop_ok:

        movea.l con_io,a1
        move.w  #CMD_WRITE,IO_COMMAND(a1)
        lea     con_msg(pc),a0
        move.l  a0,IO_DATA(a1)
        move.l  #con_msg_end-con_msg,IO_LENGTH(a1)
        jsr     LVO_DoIO(a6)
        tst.b   d0
        beq.s   .cwr_ok
        lea     s_fcwr(pc),a1
        bra     do_fail
.cwr_ok:
        movea.l con_io,a1
        move.l  IO_ACTUAL(a1),d0
        cmp.l   #con_msg_end-con_msg,d0
        beq.s   .cact_ok
        lea     s_fcact(pc),a1
        bra     do_fail
.cact_ok:
        movea.l con_io,a1
        jsr     LVO_CloseDevice(a6)

; ---- serial.device must decline --------------------------------------
        movea.l reply_port,a0
        moveq   #IOSTD_SIZE,d0
        jsr     LV_CreateIOReq(a6)
        move.l  d0,ser_io
        bne.s   .sio_ok
        lea     s_fio(pc),a1
        bra     do_fail
.sio_ok:
        lea     ser_name(pc),a0
        moveq   #0,d0
        movea.l ser_io,a1
        moveq   #0,d1
        jsr     LVO_OpenDevice(a6)
        tst.l   d0
        bne.s   .ser_ok                 ; nonzero return = failed (good)
        lea     s_fser(pc),a1
        bra     do_fail
.ser_ok:

; ---- keyboard.device: pending read + AbortIO -------------------------
        movea.l reply_port,a0
        moveq   #IOSTD_SIZE,d0
        jsr     LV_CreateIOReq(a6)
        move.l  d0,kbd_io
        bne.s   .kio_ok
        lea     s_fio(pc),a1
        bra     do_fail
.kio_ok:
        lea     kbd_name(pc),a0
        moveq   #0,d0
        movea.l kbd_io,a1
        moveq   #0,d1
        jsr     LVO_OpenDevice(a6)
        tst.l   d0
        beq.s   .kop_ok
        lea     s_fkbd(pc),a1
        bra     do_fail
.kop_ok:

        movea.l kbd_io,a1
        move.w  #KBD_READEVENT,IO_COMMAND(a1)
        lea     ie_buf(pc),a0
        move.l  a0,IO_DATA(a1)
        move.l  #IE_SIZE,IO_LENGTH(a1)
        jsr     LVO_SendIO(a6)

        ; no input pending -> device holds the request
        movea.l kbd_io,a1
        jsr     LVO_CheckIO(a6)
        tst.l   d0
        beq.s   .kpend_ok
        lea     s_fkpend(pc),a1
        bra     do_fail
.kpend_ok:

        movea.l kbd_io,a1
        jsr     LVO_AbortIO(a6)
        movea.l kbd_io,a1
        jsr     LVO_WaitIO(a6)
        cmp.b   #IOERR_ABORTED,d0
        beq.s   .kab_ok
        lea     s_fkab(pc),a1
        bra     do_fail
.kab_ok:
        movea.l kbd_io,a1
        jsr     LVO_CloseDevice(a6)

; ---- done -------------------------------------------------------------
        movea.l timer_io,a0
        jsr     LV_DeleteIOReq(a6)
        movea.l con_io,a0
        jsr     LV_DeleteIOReq(a6)
        movea.l ser_io,a0
        jsr     LV_DeleteIOReq(a6)
        movea.l kbd_io,a0
        jsr     LV_DeleteIOReq(a6)
        movea.l reply_port,a0
        jsr     LV_DeleteMsgPort(a6)

        lea     s_pass(pc),a1
        bsr     print
        moveq   #0,d0
        rts
.early_exit:
        moveq   #10,d0
        rts

do_fail:
        bsr     print
        moveq   #10,d0
        rts

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

; ---- data ------------------------------------------------------------

        even
exec_base:  dc.l 0
dos_base:   dc.l 0
reply_port: dc.l 0
timer_io:   dc.l 0
con_io:     dc.l 0
ser_io:     dc.l 0
kbd_io:     dc.l 0
ie_buf:     dcb.b IE_SIZE,0

dos_name:   dc.b "dos.library",0
timer_name: dc.b "timer.device",0
con_name:   dc.b "console.device",0
ser_name:   dc.b "serial.device",0
kbd_name:   dc.b "keyboard.device",0
con_msg:    dc.b "DEVIOTEST console.device CMD_WRITE ok",10
con_msg_end:
        even

s_start:    dc.b "DEVIOTEST START",10,0
s_tmr:      dc.b "DEVIOTEST timer.device ok",10,0
s_pass:     dc.b "DEVIOTEST PASS",10,0
s_fport:    dc.b "DEVIOTEST FAIL CreateMsgPort",10,0
s_fio:      dc.b "DEVIOTEST FAIL CreateIORequest",10,0
s_fopen:    dc.b "DEVIOTEST FAIL OpenDevice timer",10,0
s_fchk:     dc.b "DEVIOTEST FAIL CheckIO pending",10,0
s_freply:   dc.b "DEVIOTEST FAIL reply port message",10,0
s_fchk2:    dc.b "DEVIOTEST FAIL CheckIO done",10,0
s_ferr:     dc.b "DEVIOTEST FAIL timer io_Error",10,0
s_fsys:     dc.b "DEVIOTEST FAIL TR_GETSYSTIME",10,0
s_fsecs:    dc.b "DEVIOTEST FAIL tv_secs zero",10,0
s_fcon:     dc.b "DEVIOTEST FAIL OpenDevice console",10,0
s_fcwr:     dc.b "DEVIOTEST FAIL CMD_WRITE",10,0
s_fcact:    dc.b "DEVIOTEST FAIL io_Actual",10,0
s_fser:     dc.b "DEVIOTEST FAIL serial open",10,0
s_fkbd:     dc.b "DEVIOTEST FAIL OpenDevice keyboard",10,0
s_fkpend:   dc.b "DEVIOTEST FAIL kbd pending",10,0
s_fkab:     dc.b "DEVIOTEST FAIL kbd abort",10,0
        even
