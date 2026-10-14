; uaostest.device — real hunk-format test device for the UAOS-306
; on-demand .device loader.
;
; Assembled with vasm -Fhunk and linked with vlink -bamigahunk into a
; genuine AmigaOS device binary: a RomTag with RTF_AUTOINIT and
; rt_Type = NT_DEVICE, a word-offset AUTOINIT function table and an
; InitStruct data table — no UAOS-private wrapper.
;
; Vectors (real Exec device table — BeginIO/AbortIO at -30/-36):
;   -6   Open     unit (d0) must be 0 or 1 -> io_Unit, OpenCnt++
;                 else io_Error = IOERR_OPENFAIL
;   -12  Close    OpenCnt--, return 0
;   -18  Expunge  return 0
;   -24  ExtFunc  return 0
;   -30  BeginIO  command 32 (ECHO): io_Actual = io_Offset*2
;                 unknown command -> io_Error = IOERR_NOCMD, complete
;                 unit 0 completes inside BeginIO (ReplyMsg unless QUICK);
;                 unit 1 parks the request — the VERTB interrupt server
;                 replies it after a 5-tick countdown
;   -36  AbortIO  a parked request gets IOERR_ABORTED + ReplyMsg
;
; The init routine installs a struct Interrupt (data hunk) on INTB_VERTB
; via exec/AddIntServer and enables VERTB in INTENA.  The server acks
; INTREQ itself — UAOS interrupt dispatch is level-triggered, so a
; server that does not clear the bit would be re-entered forever.

IO_DEVICE    equ 20
IO_UNIT      equ 24
IO_COMMAND   equ 28
IO_FLAGS     equ 30
IO_ERROR     equ 31
IO_ACTUAL    equ 32
IO_OFFSET    equ 44
LN_TYPE      equ 8

NT_MESSAGE   equ 5
IOF_QUICK    equ 1
IOERR_OPENFAIL equ -1
IOERR_ABORTED  equ -2
IOERR_NOCMD    equ -3
CMD_ECHO     equ 32
DEFER_TICKS  equ 5

EXEC_ADDINTSERVER equ -168
EXEC_REPLYMSG     equ -378
INTB_VERTB        equ 5

LIB_OPENCNT  equ 32

        section code,code

; ---- executable entry (non-resident loaders): return -1 ---------------
        moveq   #-1,d0
        rts

; ---- Resident (RomTag) --------------------------------------------------
        even
_romtag:
        dc.w    $4afc                   ; RT_MATCHWORD
        dc.l    _romtag                 ; RT_MATCHTAG -> self
        dc.l    _endcode                ; RT_ENDSKIP
        dc.b    $80                     ; RT_FLAGS = RTF_AUTOINIT
        dc.b    2                       ; RT_VERSION
        dc.b    3                       ; RT_TYPE = NT_DEVICE
        dc.b    0                       ; RT_PRI
        dc.l    _devname                ; RT_NAME
        dc.l    _idstr                  ; RT_IDSTRING
        dc.l    _inittab                ; RT_INIT

_devname:   dc.b "uaostest.device",0
        even
_idstr:     dc.b "uaostest 2.0 (UAOS device loader test)",13,10,0
        even

; ---- InitTable: dataSize, funcTable, dataTable, initFunc -----------------
_inittab:
        dc.l    48                      ; dataSize — covers OpenCnt + seg slot
        dc.l    _functab
        dc.l    _datatab
        dc.l    _DevInit

; ---- AUTOINIT word-offset function table ----------------------------------
_functab:
        dc.w    -1
        dc.w    _Open-_functab          ; LVO -6
        dc.w    _Close-_functab         ; LVO -12
        dc.w    _Expunge-_functab       ; LVO -18
        dc.w    _ExtFunc-_functab       ; LVO -24
        dc.w    _BeginIO-_functab       ; LVO -30
        dc.w    _AbortIO-_functab       ; LVO -36
        dc.w    -1

; ---- InitStruct data table --------------------------------------------------
_datatab:
        dc.b    $a0,8                   ; INITBYTE ln_Type
        dc.b    3,0                     ;   = NT_DEVICE
        dc.b    $80,10                  ; INITLONG ln_Name
        dc.l    _devname
        dc.b    $a0,14                  ; INITBYTE lib_Flags
        dc.b    6,0                     ;   = LIBF_SUMUSED|LIBF_CHANGED
        dc.b    $90,20                  ; INITWORD lib_Version
        dc.w    2
        dc.b    $90,22                  ; INITWORD lib_Revision
        dc.w    0
        dc.b    $80,24                  ; INITLONG lib_IdString
        dc.l    _idstr
        dc.l    0                       ; InitStruct terminator

; ---- init routine: entered d0 = dev base, a0 = segList, a6 = SysBase ----
_DevInit:
        movem.l d2-d3/a2-a4,-(sp)
        move.l  d0,a4                   ; a4 = device base
        move.l  a4,_devbase
        move.l  a0,34(a4)               ; stash seglist at base+34
        ; fill the Interrupt node (data hunk): is_Data = base, is_Code set
        ; statically, node fields pre-initialised below
        lea     _intr,a2
        move.l  a4,14(a2)               ; is_Data = device base
        ; AddIntServer(INTB_VERTB, intr)
        moveq   #INTB_VERTB,d0
        move.l  a2,a1
        jsr     EXEC_ADDINTSERVER(a6)
        ; INTENA = $C020 — set-enable + VERTB
        move.w  #$c020,$dff09a
        move.l  a4,d0                   ; MakeLibrary returns our base
        movem.l (sp)+,d2-d3/a2-a4
        rts

; ---- vectors -------------------------------------------------------------
_Open:                                  ; -6: vet unit, OpenCnt++
        cmp.l   #1,d0                   ; units 0/1 only
        bhi.s   .op_fail
        move.l  d0,IO_UNIT(a1)
        addq.w  #1,LIB_OPENCNT(a6)
        move.l  a6,d0
        rts
.op_fail:
        move.b  #IOERR_OPENFAIL,IO_ERROR(a1)
        moveq   #0,d0
        rts

_Close:                                 ; -12: OpenCnt--, stay resident
        subq.w  #1,LIB_OPENCNT(a6)
        moveq   #0,d0
        rts

_Expunge:                               ; -18
_ExtFunc:                               ; -24
        moveq   #0,d0
        rts

_BeginIO:                               ; -30: A1 = ioreq, A6 = dev base
        cmp.w   #CMD_ECHO,IO_COMMAND(a1)
        beq.s   .echo
        move.b  #IOERR_NOCMD,IO_ERROR(a1)
        bra.s   .done
.echo:
        tst.l   IO_UNIT(a1)             ; unit 1 defers to the VERTB server
        bne.s   .defer
        move.l  IO_OFFSET(a1),d0
        add.l   d0,d0
        move.l  d0,IO_ACTUAL(a1)        ; ECHO: actual = offset * 2
        clr.b   IO_ERROR(a1)
.done:
        ; complete now: QUICK requests just return; async ones ReplyMsg
        btst    #0,IO_FLAGS(a1)
        bne.s   .ret
        move.l  a6,-(sp)
        movea.l 4.w,a6
        jsr     EXEC_REPLYMSG(a6)       ; ReplyMsg(a1)
        move.l  (sp)+,a6
.ret:
        rts
.defer:
        ; park the request — the interrupt server replies it when the
        ; countdown expires.  ln_Type=NT_MESSAGE marks it in-flight.
        move.l  a1,_pending
        move.l  #DEFER_TICKS,_countdown
        and.b   #$fe,IO_FLAGS(a1)       ; clear IOF_QUICK
        move.b  #NT_MESSAGE,LN_TYPE(a1)
        rts

_AbortIO:                               ; -36: abort a parked request
        move.l  _pending,d0
        beq.s   .ab_out
        cmp.l   a1,d0
        bne.s   .ab_out
        clr.l   _pending
        move.b  #IOERR_ABORTED,IO_ERROR(a1)
        move.l  a6,-(sp)
        movea.l 4.w,a6
        jsr     EXEC_REPLYMSG(a6)       ; ReplyMsg(a1)
        move.l  (sp)+,a6
.ab_out:
        moveq   #0,d0
        rts

; ---- VERTB interrupt server -------------------------------------------------
; Entered with the Exec interrupt convention (A1 = is_Data = device base,
; A6 = SysBase).  Runs the deferred unit-1 completion: count down, then
; fill the ECHO result and ReplyMsg the request.
_Server:
        move.l  _pending,d1
        beq.s   .sv_none
        subq.l  #1,_countdown
        bne.s   .sv_none
        move.l  d1,a0                   ; a0 = parked ioreq
        move.l  IO_OFFSET(a0),d0
        add.l   d0,d0
        move.l  d0,IO_ACTUAL(a0)
        clr.b   IO_ERROR(a0)
        clr.l   _pending
        movem.l a1/a6,-(sp)
        move.l  a0,a1
        movea.l 4.w,a6
        jsr     EXEC_REPLYMSG(a6)       ; ReplyMsg(a1)
        movem.l (sp)+,a1/a6
.sv_none:
        ; UAOS dispatches guest servers while the INTREQ bit is pending
        ; (level-triggered) — ack VERTB in INTREQ or we are re-entered
        ; on the next delivery pass forever.
        move.w  #$0020,$dff09c
        moveq   #0,d0
        rts

        even
_endcode:

; ---- DATA hunk -------------------------------------------------------------
        section data,data
        even
_devbase:   dc.l    0
; struct Interrupt: is_Node (14) + is_Data (14) + is_Code (18)
_intr:      dc.l    0                   ; ln_Succ
            dc.l    0                   ; ln_Pred
            dc.b    2                   ; ln_Type = NT_INTERRUPT
            dc.b    0                   ; ln_Pri
            dc.l    _devname            ; ln_Name
            dc.l    0                   ; is_Data — init fills the base
            dc.l    _Server             ; is_Code (cross-hunk RELOC32)

; ---- BSS hunk --------------------------------------------------------------
        section bss,bss
        even
_pending:   ds.l    1                   ; parked unit-1 ioreq
_countdown: ds.l    1                   ; VERTB ticks until completion

        end
