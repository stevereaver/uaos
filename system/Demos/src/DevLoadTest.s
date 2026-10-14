; DevLoadTest.s — UAOS-306 acceptance: real hunk-format .device loading.
;
; Exercises exec.library/OpenDevice resolving DEVS:uaostest.device —
; a genuine Amiga hunk-format device (RomTag + RTF_AUTOINIT InitTable,
; rt_Type = NT_DEVICE, word-offset function table, code/data/bss hunks):
;
;   OpenDevice -> LoadSeg -> RomTag scan -> InitResident -> MakeLibrary
;   -> init routine (AddIntServer on INTB_VERTB) -> AddDevice
;   -> device Open vector
;
; Checks (in order):
;   OpenDevice(unit 5) == -1, io_Device cleared      (guest Open refuses)
;   FindName(&SysBase->DeviceList, name) != 0        (loaded despite fail)
;   OpenDevice(unit 0) == 0, version 2, OpenCnt 1
;   DoIO(ECHO 21) -> io_Actual 42, QUICK kept, no port reply
;   SendIO(ECHO 5) -> CheckIO!=0, ln_Type=NT_REPLYMSG, WaitIO -> 10
;   DoIO(cmd 99) -> D0 == -3 (IOERR_NOCMD, sign-extended)
;   OpenDevice(unit 1) -> OpenCnt 2
;   SendIO(unit1 ECHO 7) -> CheckIO==0, ln_Type=NT_MESSAGE,
;                          WaitIO -> io_Actual 14 (VERTB server replied)
;   SendIO(unit1 ECHO 1) + AbortIO -> WaitIO == -2 (IOERR_ABORTED)
;   direct jsr -30(a6) on the uaostest base == BeginIO (QUICK ECHO)
;   direct jsr -30(a6) on timer.device == TR_GETSYSTIME
;   CloseDevice x2 -> OpenCnt 0, io_Device poisoned to $FFFFFFFF
;
; Prints "DEVLOAD PASS" or "DEVLOAD FAIL <stage>".

LVO_OpenLibrary  equ -552
LVO_CloseLibrary equ -414
LVO_FindName     equ -276
LVO_PutStr       equ -948
LV_RawPutChar    equ -516
LVO_OpenDevice   equ -444
LVO_CloseDevice  equ -450
LVO_DoIO         equ -456
LVO_SendIO       equ -462
LVO_CheckIO      equ -468
LVO_WaitIO       equ -474
LVO_AbortIO      equ -480
LV_GetMsg        equ -372
LV_CreateIOReq   equ -654
LV_DeleteIOReq   equ -660
LV_CreateMsgPort equ -666
LV_DeleteMsgPort equ -672

; struct Node / Message / IORequest / IOStdReq
LN_TYPE      equ 8
MN_REPLYPORT equ 14
IO_DEVICE    equ 20
IO_UNIT      equ 24
IO_COMMAND   equ 28
IO_FLAGS     equ 30
IO_ERROR     equ 31
IO_ACTUAL    equ 32
IO_OFFSET    equ 44
IOSTD_SIZE   equ 48

NT_MESSAGE   equ 5
NT_REPLYMSG  equ 7
IOF_QUICK    equ 1
IOERR_OPENFAIL equ -1
IOERR_ABORTED  equ -2
IOERR_NOCMD    equ -3

SB_DEVLIST   equ $15E

; uaostest.device
CMD_ECHO     equ 32
LIB_VERSION  equ 20
LIB_OPENCNT  equ 32

; timer.device
TR_GETSYSTIME equ 10
TR_SECS      equ 32
TR_MICRO     equ 36

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

; ---- reply port + ioreqs ------------------------------------------------
        movea.l exec_base,a6
        jsr     LV_CreateMsgPort(a6)
        move.l  d0,reply_port
        bne.s   .port_ok
        lea     s_fport(pc),a1
        bra     do_fail
.port_ok:
        movea.l reply_port,a0
        moveq   #IOSTD_SIZE,d0
        jsr     LV_CreateIOReq(a6)
        move.l  d0,io0
        beq     .iofail
        movea.l reply_port,a0
        moveq   #IOSTD_SIZE,d0
        jsr     LV_CreateIOReq(a6)
        move.l  d0,io1
        beq     .iofail
        movea.l reply_port,a0
        moveq   #IOSTD_SIZE,d0
        jsr     LV_CreateIOReq(a6)
        move.l  d0,tio
        bne.s   .ios_ok
.iofail:
        lea     s_fio(pc),a1
        bra     do_fail
.ios_ok:

; ---- 1. OpenDevice("uaostest.device",5,...) == -1 -------------------------
; First touch: loads + inits the device, runs the real guest Open vector,
; which refuses unit 5 with IOERR_OPENFAIL.  D0 must be the sign-extended
; byte (-1 as a long) and io_Device stays NULL.
        movea.l exec_base,a6
        lea     dev_name(pc),a0
        moveq   #5,d0                   ; bad unit
        movea.l io0,a1
        moveq   #0,d1
        jsr     LVO_OpenDevice(a6)
        cmp.l   #IOERR_OPENFAIL,d0
        beq.s   .u5_ok
        lea     s_fu5(pc),a1
        bra     do_fail
.u5_ok:
        movea.l io0,a1
        tst.l   IO_DEVICE(a1)
        beq.s   .u5d_ok
        lea     s_fu5d(pc),a1
        bra     do_fail
.u5d_ok:

; ---- 2. FindName(DeviceList, name) — loaded despite the failed open ------
        movea.l exec_base,a6
        move.l  a6,a0
        add.l   #SB_DEVLIST,a0
        lea     dev_name(pc),a1
        jsr     LVO_FindName(a6)
        tst.l   d0
        bne.s   .fn_ok
        lea     s_ffind(pc),a1
        bra     do_fail
.fn_ok:

; ---- 3. OpenDevice unit 0 -----------------------------------------------
        movea.l exec_base,a6
        lea     dev_name(pc),a0
        moveq   #0,d0
        movea.l io0,a1
        moveq   #0,d1
        jsr     LVO_OpenDevice(a6)
        tst.l   d0
        beq.s   .o0_ok
        lea     s_fopen0(pc),a1
        bra     do_fail
.o0_ok:
        movea.l io0,a1
        move.l  IO_DEVICE(a1),a4        ; a4 = device base
        cmpa.l  #0,a4                   ; MOVEA sets no flags — cmpa does
        bne.s   .b_ok
        lea     s_fopen0(pc),a1
        bra     do_fail
.b_ok:
        cmp.w   #2,LIB_VERSION(a4)
        beq.s   .v2_ok
        lea     s_fver(pc),a1
        bra     do_fail
.v2_ok:
        cmp.w   #1,LIB_OPENCNT(a4)
        beq.s   .oc1_ok
        lea     s_foc1(pc),a1
        bra     do_fail
.oc1_ok:
        movea.l io0,a1
        tst.l   IO_UNIT(a1)
        beq.s   .un_ok
        lea     s_funit(pc),a1
        bra     do_fail
.un_ok:

; ---- 4. DoIO(ECHO, offset 21) -> io_Actual 42 ----------------------------
        movea.l io0,a1
        move.w  #CMD_ECHO,IO_COMMAND(a1)
        move.l  #21,IO_OFFSET(a1)
        movea.l exec_base,a6
        jsr     LVO_DoIO(a6)
        tst.l   d0
        beq.s   .d0_ok
        lea     s_fdoio(pc),a1
        bra     do_fail
.d0_ok:
        movea.l io0,a1
        cmp.l   #42,IO_ACTUAL(a1)
        beq.s   .act_ok
        lea     s_fact(pc),a1
        bra     do_fail
.act_ok:
        btst    #0,IO_FLAGS(a1)         ; IOF_QUICK must still be set
        bne.s   .qk_ok
        lea     s_fqck(pc),a1
        bra     do_fail
.qk_ok:
        movea.l reply_port,a0
        jsr     LV_GetMsg(a6)
        tst.l   d0                      ; a QUICK finish replies nothing
        beq.s   .ng_ok
        lea     s_fgetm(pc),a1
        bra     do_fail
.ng_ok:

; ---- 5. SendIO(ECHO 5) — async complete inside BeginIO -------------------
        movea.l io0,a1
        move.w  #CMD_ECHO,IO_COMMAND(a1)
        move.l  #5,IO_OFFSET(a1)
        jsr     LVO_SendIO(a6)
        movea.l io0,a1
        jsr     LVO_CheckIO(a6)
        tst.l   d0                      ; replied already -> complete
        bne.s   .c5_ok
        lea     s_fchk5(pc),a1
        bra     do_fail
.c5_ok:
        movea.l io0,a1
        cmp.b   #NT_REPLYMSG,LN_TYPE(a1)
        beq.s   .lt7_ok
        lea     s_flnt7(pc),a1
        bra     do_fail
.lt7_ok:
        movea.l io0,a1
        jsr     LVO_WaitIO(a6)
        tst.l   d0
        beq.s   .w5_ok
        lea     s_fwait5(pc),a1
        bra     do_fail
.w5_ok:
        movea.l io0,a1
        cmp.l   #10,IO_ACTUAL(a1)
        beq.s   .a10_ok
        lea     s_fact5(pc),a1
        bra     do_fail
.a10_ok:
        movea.l reply_port,a0
        jsr     LV_GetMsg(a6)           ; WaitIO consumed the reply
        tst.l   d0
        beq.s   .ng5_ok
        lea     s_fgetm5(pc),a1
        bra     do_fail
.ng5_ok:

; ---- 6. DoIO(cmd 99) -> -3 (IOERR_NOCMD, sign-extended) ------------------
        movea.l io0,a1
        move.w  #99,IO_COMMAND(a1)
        jsr     LVO_DoIO(a6)
        cmp.l   #IOERR_NOCMD,d0
        beq.s   .nc_ok
        lea     s_fnocmd(pc),a1
        bra     do_fail
.nc_ok:

; ---- 7. OpenDevice unit 1 -> OpenCnt 2 -----------------------------------
        movea.l exec_base,a6
        lea     dev_name(pc),a0
        moveq   #1,d0
        movea.l io1,a1
        moveq   #0,d1
        jsr     LVO_OpenDevice(a6)
        tst.l   d0
        beq.s   .o1_ok
        lea     s_fopen1(pc),a1
        bra     do_fail
.o1_ok:
        cmp.w   #2,LIB_OPENCNT(a4)
        beq.s   .oc2_ok
        lea     s_foc2(pc),a1
        bra     do_fail
.oc2_ok:

; ---- 8. SendIO(unit1 ECHO 7) — deferred to the VERTB server --------------
        movea.l io1,a1
        move.w  #CMD_ECHO,IO_COMMAND(a1)
        move.l  #7,IO_OFFSET(a1)
        jsr     LVO_SendIO(a6)
        movea.l io1,a1
        jsr     LVO_CheckIO(a6)         ; parked -> still in-flight
        tst.l   d0
        beq.s   .pnd_ok
        lea     s_fpend(pc),a1
        bra     do_fail
.pnd_ok:
        movea.l io1,a1
        cmp.b   #NT_MESSAGE,LN_TYPE(a1)
        beq.s   .lt5_ok
        lea     s_flnt5(pc),a1
        bra     do_fail
.lt5_ok:
        movea.l io1,a1
        jsr     LVO_WaitIO(a6)          ; VERTB server replies after 5 ticks
        tst.l   d0
        beq.s   .wd_ok
        lea     s_fwaitd(pc),a1
        bra     do_fail
.wd_ok:
        movea.l io1,a1
        cmp.l   #14,IO_ACTUAL(a1)
        beq.s   .a14_ok
        lea     s_factd(pc),a1
        bra     do_fail
.a14_ok:

; ---- 9. SendIO(unit1 ECHO 1) + AbortIO -> WaitIO == -2 --------------------
        movea.l io1,a1
        move.w  #CMD_ECHO,IO_COMMAND(a1)
        move.l  #1,IO_OFFSET(a1)
        jsr     LVO_SendIO(a6)
        movea.l io1,a1
        jsr     LVO_AbortIO(a6)
        movea.l io1,a1
        jsr     LVO_WaitIO(a6)
        cmp.l   #IOERR_ABORTED,d0
        beq.s   .ab_ok
        lea     s_fabort(pc),a1
        bra     do_fail
.ab_ok:

; ---- 10. amiga.lib-style direct BeginIO: jsr -30(a6) ----------------------
        movea.l io0,a1
        move.w  #CMD_ECHO,IO_COMMAND(a1)
        move.l  #33,IO_OFFSET(a1)
        move.b  #IOF_QUICK,IO_FLAGS(a1)
        move.l  IO_DEVICE(a1),a6        ; a6 = device base, like amiga.lib
        jsr     -30(a6)
        movea.l io0,a1
        tst.b   IO_ERROR(a1)
        beq.s   .db_ok
        lea     s_fbio(pc),a1
        bra     do_fail
.db_ok:
        cmp.l   #66,IO_ACTUAL(a1)
        beq.s   .dbact_ok
        lea     s_fbioa(pc),a1
        bra     do_fail
.dbact_ok:

; ---- 11. same LVO fix on a built-in device: timer jsr -30 -----------------
        movea.l exec_base,a6
        lea     timer_name(pc),a0
        moveq   #0,d0                   ; UNIT_MICROHZ
        movea.l tio,a1
        moveq   #0,d1
        jsr     LVO_OpenDevice(a6)
        tst.l   d0
        beq.s   .top_ok
        lea     s_ftopen(pc),a1
        bra     do_fail
.top_ok:
        movea.l tio,a1
        move.w  #TR_GETSYSTIME,IO_COMMAND(a1)
        move.b  #IOF_QUICK,IO_FLAGS(a1)
        move.l  IO_DEVICE(a1),a6
        jsr     -30(a6)                 ; real BeginIO LVO on a native dev
        movea.l tio,a1
        tst.b   IO_ERROR(a1)
        beq.s   .te_ok
        lea     s_ftbio(pc),a1
        bra     do_fail
.te_ok:
        move.l  TR_SECS(a1),d0
        or.l    TR_MICRO(a1),d0
        bne.s   .ttv_ok
        lea     s_fttv(pc),a1
        bra     do_fail
.ttv_ok:
        movea.l exec_base,a6
        movea.l tio,a1
        jsr     LVO_CloseDevice(a6)

; ---- 12. CloseDevice both uaostest ioreqs --------------------------------
        movea.l exec_base,a6
        movea.l io0,a1
        jsr     LVO_CloseDevice(a6)
        movea.l io1,a1
        jsr     LVO_CloseDevice(a6)
        cmp.w   #0,LIB_OPENCNT(a4)
        beq.s   .oc0_ok
        lea     s_foc0(pc),a1
        bra     do_fail
.oc0_ok:
        movea.l io0,a1
        cmp.l   #$ffffffff,IO_DEVICE(a1)
        beq.s   .poi_ok
        lea     s_fpois(pc),a1
        bra     do_fail
.poi_ok:

; ---- cleanup + PASS -------------------------------------------------------
        movea.l io0,a0
        jsr     LV_DeleteIOReq(a6)
        movea.l io1,a0
        jsr     LV_DeleteIOReq(a6)
        movea.l tio,a0
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
io0:        dc.l 0
io1:        dc.l 0
tio:        dc.l 0

dos_name:   dc.b "dos.library",0
dev_name:   dc.b "uaostest.device",0
timer_name: dc.b "timer.device",0

s_start:    dc.b "DEVLOAD begin",10,0
s_pass:     dc.b "DEVLOAD PASS",10,0

s_fport:    dc.b "DEVLOAD FAIL CreateMsgPort",10,0
s_fio:      dc.b "DEVLOAD FAIL CreateIORequest",10,0
s_fu5:      dc.b "DEVLOAD FAIL unit5 err",10,0
s_fu5d:     dc.b "DEVLOAD FAIL unit5 io_Device",10,0
s_ffind:    dc.b "DEVLOAD FAIL DeviceList FindName",10,0
s_fopen0:   dc.b "DEVLOAD FAIL OpenDevice unit0",10,0
s_fver:     dc.b "DEVLOAD FAIL lib_Version",10,0
s_foc1:     dc.b "DEVLOAD FAIL OpenCnt 1",10,0
s_funit:    dc.b "DEVLOAD FAIL io_Unit",10,0
s_fdoio:    dc.b "DEVLOAD FAIL DoIO err",10,0
s_fact:     dc.b "DEVLOAD FAIL io_Actual 42",10,0
s_fqck:     dc.b "DEVLOAD FAIL IOF_QUICK cleared",10,0
s_fgetm:    dc.b "DEVLOAD FAIL stray reply",10,0
s_fchk5:    dc.b "DEVLOAD FAIL CheckIO send5",10,0
s_flnt7:    dc.b "DEVLOAD FAIL ln_Type reply",10,0
s_fwait5:   dc.b "DEVLOAD FAIL WaitIO send5",10,0
s_fact5:    dc.b "DEVLOAD FAIL io_Actual 10",10,0
s_fgetm5:   dc.b "DEVLOAD FAIL reply left on port",10,0
s_fnocmd:   dc.b "DEVLOAD FAIL IOERR_NOCMD",10,0
s_fopen1:   dc.b "DEVLOAD FAIL OpenDevice unit1",10,0
s_foc2:     dc.b "DEVLOAD FAIL OpenCnt 2",10,0
s_fpend:    dc.b "DEVLOAD FAIL deferred CheckIO",10,0
s_flnt5:    dc.b "DEVLOAD FAIL ln_Type message",10,0
s_fwaitd:   dc.b "DEVLOAD FAIL WaitIO deferred",10,0
s_factd:    dc.b "DEVLOAD FAIL io_Actual 14",10,0
s_fabort:   dc.b "DEVLOAD FAIL AbortIO",10,0
s_fbio:     dc.b "DEVLOAD FAIL direct BeginIO",10,0
s_fbioa:    dc.b "DEVLOAD FAIL direct BeginIO actual",10,0
s_ftopen:   dc.b "DEVLOAD FAIL OpenDevice timer",10,0
s_ftbio:    dc.b "DEVLOAD FAIL timer jsr -30",10,0
s_fttv:     dc.b "DEVLOAD FAIL timer timeval",10,0
s_foc0:     dc.b "DEVLOAD FAIL OpenCnt 0",10,0
s_fpois:    dc.b "DEVLOAD FAIL io_Device poison",10,0
        even

        end
