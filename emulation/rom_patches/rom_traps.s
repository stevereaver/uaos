; rom_traps.s — UAOS M68k Firmware Breakout Trap Stubs
;
; Syntax  : Motorola 68000 — compatible with Vasm (mot syntax),
;           Devpac 3, and AsmOne.
; Assemble: vasmm68k_mot -Fhunk -o rom_traps.o rom_traps.s
;           (or: m68k-amigaos-as -o rom_traps.o rom_traps.s)
;
; Each stub is a 6-byte breakout sequence inserted into the AROS replacement
; ROM jump table in place of the original Exec library vector.  When the
; M68k JIT core executes the ILLEGAL opcode it calls the host-side handler
; UAOS_Bridge_IllegalOpcode(), which validates the $414D signature token and
; dispatches to the matching native x86_64 C implementation.
;
; Breakout sequence layout (6 bytes):
;   $4AFC   ILLEGAL  — triggers JIT breakout callback
;   $414D   "AM"     — UAOS magic signature token
;   $NNNN            — 16-bit function index (1-based)
;   $4E75   RTS      — returns to caller after host dispatch
;
; Amiga ABI register usage per stub:
;   OpenLibrary  : A1=libname ptr, D0=version → result in D0
;   AllocMem     : D0=byteSize,   D1=requirements → result in D0
;   FreeMem      : A1=memBlock,   D0=byteSize
;   CloseLibrary : A1=library ptr
;   FindTask     : A1=name ptr (NULL=current task) → result in D0
;   AddTask      : A1=task, A2=initialPC, A3=finalPC → result in D0
;   RemTask      : A1=task ptr (NULL=self)
;   Wait         : D0=signalMask → result in D0
;   Signal       : A1=task, D0=signalSet
;   SetFunction  : A1=library, A0=funcOffset, D0=funcEntry → result in D0

; =========================================================================
; Export all stub labels so the ROM patcher (uaos_uae_bridge.c) can locate
; and inject each one into the correct Exec jump table slot.
; =========================================================================

        XDEF    _UAOS_Thunk_OpenLibrary
        XDEF    _UAOS_Thunk_AllocMem
        XDEF    _UAOS_Thunk_FreeMem
        XDEF    _UAOS_Thunk_CloseLibrary
        XDEF    _UAOS_Thunk_FindTask
        XDEF    _UAOS_Thunk_AddTask
        XDEF    _UAOS_Thunk_RemTask
        XDEF    _UAOS_Thunk_Wait
        XDEF    _UAOS_Thunk_Signal
        XDEF    _UAOS_Thunk_SetFunction
        XDEF    _UAOS_TrapTable

; =========================================================================
; MK_THUNK macro
;
; Parameters:
;   \1  — UAOS magic signature word  (always $414D)
;   \2  — 16-bit function index       (1 = OpenLibrary, 2 = AllocMem, …)
;
; Emits exactly 8 bytes:
;   dc.w $4AFC   ILLEGAL opcode
;   dc.w \1      Magic token  ($414D)
;   dc.w \2      Function index
;   rts          Return to guest caller
; =========================================================================

MK_THUNK MACRO
        ILLEGAL                 ; dc.w $4AFC — triggers JIT breakout
        dc.w    \1              ; UAOS magic signature ($414D = "AM")
        dc.w    \2              ; function index ID
        rts                     ; hand control back to guest after dispatch
        ENDM

; =========================================================================
; Code section
; =========================================================================

        SECTION code,CODE

; -------------------------------------------------------------------------
; Exec / exec.library stubs
; -------------------------------------------------------------------------

_UAOS_Thunk_OpenLibrary:
        MK_THUNK $414D,1        ; A1=libname, D0=version → D0=lib base

_UAOS_Thunk_AllocMem:
        MK_THUNK $414D,2        ; D0=byteSize, D1=requirements → D0=ptr

_UAOS_Thunk_FreeMem:
        MK_THUNK $414D,3        ; A1=memBlock, D0=byteSize

_UAOS_Thunk_CloseLibrary:
        MK_THUNK $414D,4        ; A1=library ptr

_UAOS_Thunk_FindTask:
        MK_THUNK $414D,5        ; A1=name ptr (NULL=self) → D0=task ptr

_UAOS_Thunk_AddTask:
        MK_THUNK $414D,6        ; A1=task, A2=initialPC, A3=finalPC → D0

_UAOS_Thunk_RemTask:
        MK_THUNK $414D,7        ; A1=task ptr (NULL=self)

_UAOS_Thunk_Wait:
        MK_THUNK $414D,8        ; D0=signalMask → D0=received signals

_UAOS_Thunk_Signal:
        MK_THUNK $414D,9        ; A1=task, D0=signalSet

_UAOS_Thunk_SetFunction:
        MK_THUNK $414D,10       ; A1=lib, A0=offset, D0=newFunc → D0=oldFunc

; =========================================================================
; Trap dispatch table — consumed by uaos_uae_bridge.c at ROM patch time.
; Layout: pairs of (longword stub address, word function index), terminated
; by a zero longword sentinel.
; =========================================================================

        SECTION data,DATA

_UAOS_TrapTable:
        dc.l    _UAOS_Thunk_OpenLibrary
        dc.w    1
        dc.l    _UAOS_Thunk_AllocMem
        dc.w    2
        dc.l    _UAOS_Thunk_FreeMem
        dc.w    3
        dc.l    _UAOS_Thunk_CloseLibrary
        dc.w    4
        dc.l    _UAOS_Thunk_FindTask
        dc.w    5
        dc.l    _UAOS_Thunk_AddTask
        dc.w    6
        dc.l    _UAOS_Thunk_RemTask
        dc.w    7
        dc.l    _UAOS_Thunk_Wait
        dc.w    8
        dc.l    _UAOS_Thunk_Signal
        dc.w    9
        dc.l    _UAOS_Thunk_SetFunction
        dc.w    10
        dc.l    0               ; sentinel — marks end of table
        dc.w    0

        END
