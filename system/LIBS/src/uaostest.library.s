; uaostest.library — real hunk-format test library for the UAOS-305
; on-demand .library loader.
;
; Assembled with vasm -Fhunk and linked with vlink -bamigahunk into a
; genuine AmigaOS library binary: separate CODE/DATA/BSS hunks, a RomTag
; with RTF_AUTOINIT, a word-offset AUTOINIT function table and an
; InitStruct data table — no UAOS-private wrapper.
;
; Vectors:
;   -6   Open     opencnt++, return base
;   -12  Close    opencnt--, return 0 (stay resident)
;   -18  Expunge  return 0
;   -24  ExtFunc  return 0
;   -30  Add      d0 + d1 -> d0
;   -36  GetMagic return the 'INIT' marker init wrote at base+38
;   -42  ExecProbe AllocMem(64, MEMF_CLEAR)/FreeMem through SysBase (4.w)
;   -48  GetData  return a long from the DATA hunk (forces a cross-hunk
;                 RELOC32 — value $C0DEF00D)
;   -54  Counter  increment a long in the BSS hunk and return it

MEMF_CLEAR    equ $00010000
LIB_OPENCNT   equ 32                  ; lib_OpenCnt
EXEC_ALLOCMEM equ -198
EXEC_FREEMEM  equ -210

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
        dc.b    3                       ; RT_VERSION
        dc.b    9                       ; RT_TYPE = NT_LIBRARY
        dc.b    0                       ; RT_PRI
        dc.l    _libname                ; RT_NAME
        dc.l    _idstr                  ; RT_IDSTRING
        dc.l    _inittab                ; RT_INIT

_libname:   dc.b "uaostest.library",0
        even
_idstr:     dc.b "uaostest 3.1 (UAOS loader test)",13,10,0
        even

; ---- InitTable: dataSize, funcTable, dataTable, initFunc -----------------
_inittab:
        dc.l    46                      ; dataSize — covers base+38 marker
        dc.l    _functab
        dc.l    _datatab
        dc.l    _LibInit

; ---- AUTOINIT word-offset function table ----------------------------------
; A leading -1 word selects the displacement form: each entry is the
; WORD offset of the function relative to _functab itself, -1 terminated.
_functab:
        dc.w    -1
        dc.w    _Open-_functab          ; LVO -6
        dc.w    _Close-_functab         ; LVO -12
        dc.w    _Expunge-_functab       ; LVO -18
        dc.w    _ExtFunc-_functab       ; LVO -24
        dc.w    _Add-_functab           ; LVO -30
        dc.w    _GetMagic-_functab      ; LVO -36
        dc.w    _ExecProbe-_functab     ; LVO -42
        dc.w    _GetData-_functab       ; LVO -48
        dc.w    _Counter-_functab       ; LVO -54
        dc.w    -1

; ---- InitStruct data table --------------------------------------------------
; Encodings: $a0 = INITBYTE off8, $90 = INITWORD off8, $80 = INITLONG off8.
_datatab:
        dc.b    $a0,8                   ; INITBYTE ln_Type
        dc.b    9,0                     ;   = NT_LIBRARY
        dc.b    $80,10                  ; INITLONG ln_Name
        dc.l    _libname
        dc.b    $a0,14                  ; INITBYTE lib_Flags
        dc.b    6,0                     ;   = LIBF_SUMUSED|LIBF_CHANGED
        dc.b    $90,20                  ; INITWORD lib_Version
        dc.w    3
        dc.b    $90,22                  ; INITWORD lib_Revision
        dc.w    1
        dc.b    $80,24                  ; INITLONG lib_IdString
        dc.l    _idstr
        dc.l    0                       ; InitStruct terminator

; ---- init routine: entered d0 = lib base, a0 = segList, a6 = SysBase ----
_LibInit:
        move.l  d0,a1
        move.l  a0,34(a1)               ; stash seglist at base+34
        move.l  #$494e4954,38(a1)       ; 'INIT' marker at base+38
        rts                             ; return the base in d0

; ---- vectors -------------------------------------------------------------
_Open:                                  ; -6: OpenCnt++, return base
        addq.w  #1,LIB_OPENCNT(a6)
        move.l  a6,d0
        rts

_Close:                                 ; -12: OpenCnt--, stay resident
        subq.w  #1,LIB_OPENCNT(a6)
        moveq   #0,d0                   ; no seglist -> never expunged
        rts

_Expunge:                               ; -18
_ExtFunc:                               ; -24
        moveq   #0,d0
        rts

_Add:                                   ; -30: d0 + d1 -> d0
        add.l   d1,d0
        rts

_GetMagic:                              ; -36: long at base+38 ('INIT')
        move.l  38(a6),d0
        rts

_ExecProbe:                             ; -42: AllocMem/FreeMem round-trip
        movem.l d1/a1/a6,-(sp)
        movea.l 4.w,a6                  ; SysBase
        move.l  #64,d0
        move.l  #MEMF_CLEAR,d1
        jsr     EXEC_ALLOCMEM(a6)
        tst.l   d0
        beq.s   .ep_fail
        move.l  d0,a1
        move.l  #64,d0
        jsr     EXEC_FREEMEM(a6)
        moveq   #1,d0
        bra.s   .ep_out
.ep_fail:
        moveq   #0,d0
.ep_out:
        movem.l (sp)+,d1/a1/a6
        rts

_GetData:                               ; -48: long from the DATA hunk —
        move.l  _DataVal,d0             ; absolute ref forces a RELOC32
        rts                             ; into a different hunk
_Counter:                               ; -54: ++long in the BSS hunk
        move.l  _counter,d0             ; absolute ref into the BSS hunk
        addq.l  #1,d0
        move.l  d0,_counter
        rts

        even
_endcode:

; ---- DATA hunk -------------------------------------------------------------
        section data,data
        even
_DataVal:   dc.l    $c0def00d

; ---- BSS hunk --------------------------------------------------------------
        section bss,bss
        even
_counter:   ds.l    1

        end
