; ExecTest.s — UAOS-239 acceptance: OS-2.x exec.library coverage.
;
; Exercises the exec LVOs added for the OS-2.x application set:
;   Supervisor, SetSR, AllocAbs, Allocate/Deallocate, AddMemList,
;   AddTask/FindTask/RemTask, Add/RemLibrary+Device+Resource,
;   semaphore list ops (Init/Add/Find/AttemptShared/ObtainList/
;   ReleaseList/Rem), MakeFunctions/MakeLibrary, pools, and the
;   trivial stubs (RawMayGetChar, CachePreDMA, SumKickData,
;   FindResident, ChildWait, Add/RemMemHandler, SumLibrary, Debug,
;   RawIOInit, RawPutChar, Alert).
;
; Prints "EXECTEST PASS" or "EXECTEST FAIL <stage>" on stdout.

LVO_OpenLibrary  equ -552
LVO_PutStr       equ -948

; exec LVOs under test
LV_Supervisor     equ -30
LV_MakeLibrary    equ -84
LV_MakeFunctions  equ -90
LV_FindResident   equ -96
LV_InitResident   equ -102
LV_Alert          equ -108
LV_Debug          equ -114
LV_SetSR          equ -144
LV_Allocate       equ -186
LV_Deallocate     equ -192
LV_AllocMem       equ -198
LV_AllocAbs       equ -204
LV_FreeMem        equ -210
LV_Remove         equ -252
LV_FindName       equ -276
LV_AddTask        equ -282
LV_RemTask        equ -288
LV_FindTask       equ -294
LV_AddLibrary     equ -396
LV_RemLibrary     equ -402
LV_SumLibrary     equ -426
LV_AddDevice      equ -432
LV_RemDevice      equ -438
LV_AddResource    equ -486
LV_RemResource    equ -492
LV_RawIOInit      equ -504
LV_RawMayGetChar  equ -510
LV_RawPutChar     equ -516
LV_GetCC          equ -528
LV_InitSemaphore  equ -558
LV_ObtainSemList  equ -582
LV_ReleaseSemList equ -588
LV_FindSemaphore  equ -594
LV_AddSemaphore   equ -600
LV_RemSemaphore   equ -606
LV_SumKickData    equ -612
LV_AddMemList     equ -618
LV_CreatePool     equ -696
LV_DeletePool     equ -702
LV_AllocPooled    equ -708
LV_FreePooled     equ -714
LV_AttemptSemSh   equ -720
LV_ChildWait      equ -756
LV_CachePreDMA    equ -762
LV_AddMemHandler  equ -774
LV_RemMemHandler  equ -780

; SysBase list offsets
SB_RESLIST   equ $150
SB_DEVLIST   equ $15E
SB_LIBLIST   equ $17A
SB_SEMLIST   equ $214
SB_MEMLIST   equ $142

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

; ---- Supervisor(-30): calls a5, rts returns here ---------------------
        movea.l exec_base,a6
        lea     sup_func(pc),a5
        jsr     LV_Supervisor(a6)
        cmp.l   #77,d0
        beq.s   .sup_ok
        lea     s_fsup(pc),a1
        bra     do_fail
.sup_ok:

; ---- SetSR(-144)+GetCC(-528): set C flag, read it back ---------------
        moveq   #1,d0                   ; C bit
        moveq   #$1F,d1                 ; CCR mask
        jsr     LV_SetSR(a6)
        jsr     LV_GetCC(a6)
        btst    #0,d0
        bne.s   .sr_ok
        lea     s_fsr(pc),a1
        bra     do_fail
.sr_ok:
        moveq   #0,d0                   ; clear CCR back
        moveq   #$1F,d1
        jsr     LV_SetSR(a6)

; ---- AllocAbs(-204): free-then-reclaim + busy-address fail -----------
        move.l  #$2000,d0
        moveq   #0,d1                   ; MEMF_ANY-ish (public)
        jsr     LV_AllocMem(a6)
        tst.l   d0
        bne.s   .am_ok
        lea     s_fallocmem(pc),a1
        bra     do_fail
.am_ok:
        move.l  d0,a2                   ; a2 = block
        move.l  a2,a1
        move.l  #$2000,d0
        jsr     LV_FreeMem(a6)
        ; AllocAbs($1000, a2) must return a2 (exact fit in freed block)
        move.l  #$1000,d0
        move.l  a2,a1
        jsr     LV_AllocAbs(a6)
        cmp.l   a2,d0
        beq.s   .abs_ok
        lea     s_fallocabs(pc),a1
        bra     do_fail
.abs_ok:
        ; split the remainder: heap header needs 8 bytes, so payload
        ; a2+$1008 puts its header at a2+$1000 (free-block start)
        move.l  #$800,d0
        move.l  a2,a1
        add.l   #$1008,a1
        jsr     LV_AllocAbs(a6)
        move.l  a2,a1
        add.l   #$1008,a1
        cmp.l   a1,d0
        beq.s   .abs2_ok
        lea     s_fallocabs2(pc),a1
        bra     do_fail
.abs2_ok:
        ; AllocAbs inside the busy block must fail
        moveq   #16,d0
        move.l  a2,a1
        add.l   #$800,a1                ; inside the first allocation
        jsr     LV_AllocAbs(a6)
        tst.l   d0
        beq.s   .abs3_ok
        lea     s_fallocabs3(pc),a1
        bra     do_fail
.abs3_ok:
        ; free both pieces
        move.l  a2,a1
        move.l  #$1000,d0
        jsr     LV_FreeMem(a6)
        move.l  a2,a1
        add.l   #$1008,a1
        move.l  #$800,d0
        jsr     LV_FreeMem(a6)

; ---- MakeFunctions(-90): build vectors, call through them ------------
        ; target = mflib+$80; vectors land below it
        movea.l exec_base,a6
        lea     mflib+$80(pc),a0
        lea     func_array(pc),a1
        suba.l  a2,a2
        jsr     LV_MakeFunctions(a6)
        ; jsr -30(mflib+$80) -> ret42 -> d0=42
        lea     mflib+$80(pc),a6
        jsr     -30(a6)
        cmp.l   #42,d0
        beq.s   .mf_ok
        lea     s_fmkfn(pc),a1
        bra     do_fail
.mf_ok:

; ---- MakeLibrary(-84): returns base with a working vector ------------
        movea.l exec_base,a6
        lea     func_array(pc),a0
        suba.l  a1,a1                   ; no structInit
        suba.l  a2,a2                   ; no init fn
        moveq   #$40,d0                 ; dataSize
        moveq   #0,d1                   ; segList
        jsr     LV_MakeLibrary(a6)
        tst.l   d0
        bne.s   .ml_ok
        lea     s_fmklib(pc),a1
        bra     do_fail
.ml_ok:
        move.l  d0,a3                   ; a3 = new lib base
        ; lib_NegSize (+16) >= 36 and a vector call returns 42
        cmp.w   #36,16(a3)
        bhs.s   .ml_szok
        lea     s_fmksize(pc),a1
        bra     do_fail
.ml_szok:
        ; verify the generated vector: JMP abs.l at base-30 -> ret42.
        ; (cannot call through it: heap PCs arm the low-reentry watchdog,
        ; which kills the task on return into this code's 0x2xxxx band)
        move.l  a3,a0
        sub.l   #30,a0
        cmp.w   #$4EF9,(a0)+
        bne.s   .ml_badvec
        lea     ret42(pc),a1
        cmp.l   (a0),a1
        bne.s   .ml_badvec
        bra.s   .ml_vecok
.ml_badvec:
        lea     s_fmkvec(pc),a1
        bra     do_fail
.ml_vecok:
        ; free: base-NegSize, size NegSize+PosSize
        movea.l exec_base,a6
        move.l  a3,a1
        moveq   #0,d0
        move.w  16(a3),d0
        sub.l   d0,a1
        add.w   18(a3),d0
        jsr     LV_FreeMem(a6)

; ---- Allocate/Deallocate on a manual MemHeader -----------------------
        ; init MemHeader at membuf: First→chunk, Free=$1C0, Lower/Upper
        lea     membuf(pc),a0
        lea     membuf+32(pc),a1
        move.l  a1,16(a0)               ; mh_First
        move.l  a1,20(a0)               ; mh_Lower
        lea     membuf+$200(pc),a1
        move.l  a1,24(a0)               ; mh_Upper
        move.l  #$1E0,28(a0)            ; mh_Free
        move.l  #0,membuf+32            ; mc_Next
        move.l  #$1E0,membuf+36         ; mc_Bytes
        movea.l exec_base,a6
        lea     membuf(pc),a0
        moveq   #$40,d0
        jsr     LV_Allocate(a6)
        tst.l   d0
        bne.s   .alloc_ok
        lea     s_fallocate(pc),a1
        bra     do_fail
.alloc_ok:
        move.l  d0,a3
        ; addr must be inside [membuf+32, membuf+$200)
        lea     membuf+32(pc),a1
        cmp.l   a1,a3
        blo.s   .alloc_bad
        lea     membuf+$200(pc),a1
        cmp.l   a1,a3
        bhs.s   .alloc_bad
        bra.s   .alloc_in
.alloc_bad:
        lea     s_fallocrng(pc),a1
        bra     do_fail
.alloc_in:
        ; Deallocate it back, mh_Free must be restored
        lea     membuf(pc),a0
        move.l  a3,a1
        moveq   #$40,d0
        jsr     LV_Deallocate(a6)
        cmp.l   #$1E0,membuf+28
        beq.s   .dealloc_ok
        lea     s_fdealloc(pc),a1
        bra     do_fail
.dealloc_ok:

; ---- AddMemList(-618): formats header + links SysBase MemList --------
        movea.l exec_base,a6
        lea     membuf2(pc),a0
        move.l  #$100,d0
        moveq   #1,d1                   ; MEMF_PUBLIC
        moveq   #0,d2                   ; pri
        lea     ml_name(pc),a3
        jsr     LV_AddMemList(a6)
        lea     membuf2(pc),a1
        cmp.l   a1,d0
        beq.s   .aml_ok
        lea     s_faddml(pc),a1
        bra     do_fail
.aml_ok:
        ; FindName(SysBase+MemList, name) finds it
        movea.l exec_base,a0
        add.l   #SB_MEMLIST,a0
        lea     ml_name(pc),a1
        jsr     LV_FindName(a6)
        lea     membuf2(pc),a1
        cmp.l   a1,d0
        beq.s   .aml_find
        lea     s_fmlfind(pc),a1
        bra     do_fail
.aml_find:
        ; Remove it back out of the system list
        lea     membuf2(pc),a1
        jsr     LV_Remove(a6)

; ---- AddLibrary/FindName/RemLibrary ----------------------------------
        lea     fakelib(pc),a1
        move.b  #9,8(a1)                ; NT_LIBRARY
        lea     fl_name(pc),a0
        move.l  a0,10(a1)               ; ln_Name
        movea.l exec_base,a6
        jsr     LV_AddLibrary(a6)
        movea.l exec_base,a0
        add.l   #SB_LIBLIST,a0
        lea     fl_name(pc),a1
        jsr     LV_FindName(a6)
        lea     fakelib(pc),a1
        cmp.l   a1,d0
        beq.s   .al_ok
        lea     s_faddlib(pc),a1
        bra     do_fail
.al_ok:
        lea     fakelib(pc),a1
        jsr     LV_RemLibrary(a6)

; ---- AddDevice / AddResource: same list plumbing ---------------------
        lea     fakedev(pc),a1
        move.b  #3,8(a1)                ; NT_DEVICE
        lea     fd_name(pc),a0
        move.l  a0,10(a1)
        movea.l exec_base,a6
        jsr     LV_AddDevice(a6)
        movea.l exec_base,a0
        add.l   #SB_DEVLIST,a0
        lea     fd_name(pc),a1
        jsr     LV_FindName(a6)
        lea     fakedev(pc),a1
        cmp.l   a1,d0
        beq.s   .ad_ok
        lea     s_fadddev(pc),a1
        bra     do_fail
.ad_ok:
        lea     fakedev(pc),a1
        jsr     LV_RemDevice(a6)

        lea     fakeres(pc),a1
        move.b  #8,8(a1)                ; NT_RESOURCE
        lea     fr_name(pc),a0
        move.l  a0,10(a1)
        movea.l exec_base,a6
        jsr     LV_AddResource(a6)
        movea.l exec_base,a0
        add.l   #SB_RESLIST,a0
        lea     fr_name(pc),a1
        jsr     LV_FindName(a6)
        lea     fakeres(pc),a1
        cmp.l   a1,d0
        beq.s   .ar_ok
        lea     s_faddres(pc),a1
        bra     do_fail
.ar_ok:
        lea     fakeres(pc),a1
        jsr     LV_RemResource(a6)

; ---- AddTask(-282)/FindTask(-294)/RemTask(-288) ----------------------
        lea     faketask(pc),a1
        move.b  #1,8(a1)                ; NT_TASK
        lea     ft_name(pc),a0
        move.l  a0,10(a1)
        movea.l exec_base,a6
        suba.l  a2,a2
        suba.l  a3,a3
        jsr     LV_AddTask(a6)
        lea     faketask(pc),a1
        cmp.l   a1,d0
        beq.s   .at_ok
        lea     s_faddtask(pc),a1
        bra     do_fail
.at_ok:
        lea     ft_name(pc),a1
        jsr     LV_FindTask(a6)
        lea     faketask(pc),a1
        cmp.l   a1,d0
        beq.s   .ft_ok
        lea     s_ffindtask(pc),a1
        bra     do_fail
.ft_ok:
        lea     faketask(pc),a1
        jsr     LV_RemTask(a6)
        ; gone from the lists now
        lea     ft_name(pc),a1
        jsr     LV_FindTask(a6)
        tst.l   d0
        beq.s   .rt_ok
        lea     s_fremtask(pc),a1
        bra     do_fail
.rt_ok:

; ---- semaphores: Init/Add/Find/AttemptShared/List/Rem ----------------
        lea     semname(pc),a0
        move.l  a0,sem+10               ; ss_Link.ln_Name
        movea.l exec_base,a6
        lea     sem(pc),a0
        jsr     LV_InitSemaphore(a6)
        lea     sem(pc),a1
        jsr     LV_AddSemaphore(a6)
        lea     semname(pc),a1
        jsr     LV_FindSemaphore(a6)
        lea     sem(pc),a1
        cmp.l   a1,d0
        beq.s   .sem_ok
        lea     s_fsem(pc),a1
        bra     do_fail
.sem_ok:
        lea     sem(pc),a0
        jsr     LV_AttemptSemSh(a6)
        tst.l   d0
        bne.s   .ash_ok
        lea     s_fsemsh(pc),a1
        bra     do_fail
.ash_ok:
        ; unlink from the system SemaphoreList before clobbering ln_Succ
        lea     sem(pc),a1
        jsr     LV_RemSemaphore(a6)
        ; MinList {head→sem, sem.succ→list+4, tail=0, tailpred→sem}
        lea     semlist(pc),a0
        lea     sem(pc),a1
        move.l  a1,(a0)+
        move.l  a0,sem                  ; sem.ln_Succ = &mlh_Tail (list+4)
        move.l  #0,(a0)+
        move.l  a1,(a0)
        lea     semlist(pc),a0
        jsr     LV_ObtainSemList(a6)
        lea     semlist(pc),a0
        jsr     LV_ReleaseSemList(a6)

; ---- pools: Create/AllocPooled/FreePooled/Delete ---------------------
        movea.l exec_base,a6
        moveq   #0,d0
        moveq   #0,d1
        moveq   #0,d2
        jsr     LV_CreatePool(a6)
        tst.l   d0
        bne.s   .pool_ok
        lea     s_fpool(pc),a1
        bra     do_fail
.pool_ok:
        move.l  d0,a3                   ; a3 = pool
        move.l  a3,a0
        move.l  #$80,d0
        jsr     LV_AllocPooled(a6)
        tst.l   d0
        bne.s   .pa_ok
        lea     s_fpalloc(pc),a1
        bra     do_fail
.pa_ok:
        move.l  d0,a4
        move.l  a3,a0
        move.l  a4,a1
        move.l  #$80,d0
        jsr     LV_FreePooled(a6)
        move.l  a3,a0
        move.l  #$40,d0
        jsr     LV_AllocPooled(a6)
        tst.l   d0
        bne.s   .pa2_ok
        lea     s_fpalloc2(pc),a1
        bra     do_fail
.pa2_ok:
        move.l  a3,a0
        jsr     LV_DeletePool(a6)

; ---- trivial stubs ----------------------------------------------------
        movea.l exec_base,a6
        jsr     LV_RawMayGetChar(a6)
        cmp.l   #-1,d0
        beq.s   .rmgc_ok
        lea     s_frmgc(pc),a1
        bra     do_fail
.rmgc_ok:
        move.l  #$1234,a0
        jsr     LV_CachePreDMA(a6)
        cmp.l   #$1234,d0
        beq.s   .pdma_ok
        lea     s_fpdma(pc),a1
        bra     do_fail
.pdma_ok:
        jsr     LV_ChildWait(a6)
        tst.l   d0
        beq.s   .cw_ok
        lea     s_fcwait(pc),a1
        bra     do_fail
.cw_ok:
        jsr     LV_SumKickData(a6)
        lea     res_name(pc),a1
        jsr     LV_FindResident(a6)     ; -> 0 expected
        lea     membuf(pc),a1
        jsr     LV_AddMemHandler(a6)    ; -> handler ptr
        lea     membuf(pc),a1
        cmp.l   a1,d0
        beq.s   .mh_ok
        lea     s_fmh(pc),a1
        bra     do_fail
.mh_ok:
        lea     membuf(pc),a1
        jsr     LV_RemMemHandler(a6)
        jsr     LV_RawIOInit(a6)
        moveq   #'X',d0
        jsr     LV_RawPutChar(a6)
        jsr     LV_Debug(a6)
        move.l  exec_base,a1
        jsr     LV_SumLibrary(a6)
        move.l  #$80000000,d7           ; recoverable alert — log only
        jsr     LV_Alert(a6)

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

sup_func:
        moveq   #77,d0
        rts

ret42:
        moveq   #42,d0
        rts

ret43:
        moveq   #43,d0
        rts

; ---- data ------------------------------------------------------------

        even
exec_base:  dc.l 0
dos_base:   dc.l 0
func_array: dc.w -30
            dc.l ret42
            dc.w -36
            dc.l ret43
            dc.w -1
            even

dos_name:   dc.b "dos.library",0
fl_name:    dc.b "exectest.library",0
fd_name:    dc.b "exectest.device",0
fr_name:    dc.b "exectest.resource",0
ft_name:    dc.b "exectest task",0
semname:    dc.b "exectest sem",0
ml_name:    dc.b "exectest memlist",0
res_name:   dc.b "exectest resident",0

s_start:    dc.b "EXECTEST begin",10,0
s_pass:     dc.b "EXECTEST PASS",10,0

s_fsup:     dc.b "EXECTEST FAIL Supervisor",10,0
s_fsr:      dc.b "EXECTEST FAIL SetSR/GetCC",10,0
s_fallocmem: dc.b "EXECTEST FAIL AllocMem",10,0
s_fallocabs: dc.b "EXECTEST FAIL AllocAbs exact",10,0
s_fallocabs2: dc.b "EXECTEST FAIL AllocAbs split",10,0
s_fallocabs3: dc.b "EXECTEST FAIL AllocAbs busy",10,0
s_fmkfn:    dc.b "EXECTEST FAIL MakeFunctions",10,0
s_fmklib:   dc.b "EXECTEST FAIL MakeLibrary NULL",10,0
s_fmksize:  dc.b "EXECTEST FAIL MakeLibrary NegSize",10,0
s_fmkvec:   dc.b "EXECTEST FAIL MakeLibrary vector",10,0
s_fallocate: dc.b "EXECTEST FAIL Allocate NULL",10,0
s_fallocrng: dc.b "EXECTEST FAIL Allocate range",10,0
s_fdealloc: dc.b "EXECTEST FAIL Deallocate",10,0
s_faddml:   dc.b "EXECTEST FAIL AddMemList",10,0
s_fmlfind:  dc.b "EXECTEST FAIL MemList FindName",10,0
s_faddlib:  dc.b "EXECTEST FAIL AddLibrary/FindName",10,0
s_fadddev:  dc.b "EXECTEST FAIL AddDevice/FindName",10,0
s_faddres:  dc.b "EXECTEST FAIL AddResource/FindName",10,0
s_faddtask: dc.b "EXECTEST FAIL AddTask",10,0
s_ffindtask: dc.b "EXECTEST FAIL FindTask(name)",10,0
s_fremtask: dc.b "EXECTEST FAIL RemTask",10,0
s_fsem:     dc.b "EXECTEST FAIL FindSemaphore",10,0
s_fsemsh:   dc.b "EXECTEST FAIL AttemptSemaphoreShared",10,0
s_fpool:    dc.b "EXECTEST FAIL CreatePool",10,0
s_fpalloc:  dc.b "EXECTEST FAIL AllocPooled",10,0
s_fpalloc2: dc.b "EXECTEST FAIL AllocPooled#2",10,0
s_frmgc:    dc.b "EXECTEST FAIL RawMayGetChar",10,0
s_fpdma:    dc.b "EXECTEST FAIL CachePreDMA",10,0
s_fcwait:   dc.b "EXECTEST FAIL ChildWait",10,0
s_fmh:      dc.b "EXECTEST FAIL AddMemHandler",10,0

        even
mflib:      ds.b $100
membuf:     ds.b $200
membuf2:    ds.b $200
fakelib:    ds.b 64
fakedev:    ds.b 64
fakeres:    ds.b 64
faketask:   ds.b 64
sem:        ds.b 64
semlist:    ds.b 16

        end
