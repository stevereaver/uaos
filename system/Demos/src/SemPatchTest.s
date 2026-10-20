; SemPatchTest.s — UAOS-365 acceptance: SetFunction old-vector chaining,
; named-semaphore cross-window install, ReadArgs parsing, PrintFault,
; MEMF_REVERSE carving.
;
; Covers the SetPatch-driven fixes:
;   T1  SetFunction on an ILLEGAL stub returns a CALLABLE old vector —
;       a wrapper that jsr's it must reach the native implementation.
;   T2  AddSemaphore registers the sem cross-window; the sem + private
;       SetPatch-style extension is left installed for SemPatchRead.
;   RA  ReadArgs("QUIET/S,LEVEL/N") parses "QUIET LEVEL=5".
;   PF  PrintFault prints "PFHDR: object not found".
;   RV  AllocMem(MEMF_FAST|MEMF_REVERSE) carves the pool top.
;
; Prints one PASS/FAIL line per check.  Launch with:
;   SYS:Demos/SemPatchTest QUIET LEVEL=5

LVO_OpenLibrary  equ -552
LVO_PutStr       equ -948
LVO_ReadArgs     equ -798
LVO_FreeArgs     equ -858
LVO_PrintFault   equ -474

LV_SetFunction   equ -420
LV_FindTask      equ -294
LV_AllocMem      equ -198
LV_InitSemaphore equ -558
LV_FindSemaphore equ -594
LV_AddSemaphore  equ -600

MEMF_PUBLIC  equ $1
MEMF_FAST    equ $4
MEMF_CLEAR   equ $10000
MEMF_REVERSE equ $40000

        section code,code

start:
        movea.l 4.w,a6
        move.l  a6,exec_base
        lea     dos_name(pc),a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,dos_base
        bne.s   .have_dos
        rts                            ; stack top holds the DOS_EXIT stub
.have_dos:

; ---- T1: SetFunction old vector is callable ---------------------------
        movea.l exec_base,a6
        lea     my_findtask(pc),a2
        move.l  a2,d0                  ; newFunc
        move.l  #LV_FindTask,a0        ; funcOffset (negative)
        movea.l exec_base,a1           ; library
        jsr     LV_SetFunction(a6)
        move.l  d0,old_findtask
        ; first patch of an ILLEGAL stub must hand back a trampoline in
        ; the 0xB000-0xC000 oldvec arena — NOT the overwritten slot
        move.l  d0,d1
        and.l   #$FFFFF000,d1
        cmp.l   #$B000,d1
        beq.s   .t1_call
        lea     s_f_t1a(pc),a1
        bra     do_fail
.t1_call:
        ; call the patched vector: FindTask(0) -> wrapper -> old -> native
        movea.l exec_base,a6
        suba.l  a1,a1
        jsr     LV_FindTask(a6)
        cmp.l   #1,wrap_count
        bne.s   .t1_fail
        tst.b   old_ok
        beq.s   .t1_fail
        ; unpatch — restore the stub by SetFunction(newFunc=old)
        movea.l exec_base,a6
        move.l  old_findtask,d0
        move.l  #LV_FindTask,a0
        movea.l exec_base,a1
        jsr     LV_SetFunction(a6)
        lea     s_t1_ok(pc),a1
        bsr     print
        bra     .t2
.t1_fail:
        lea     s_f_t1b(pc),a1
        bra     do_fail
.t2:

; ---- T2: named semaphore + SetPatch-style private extension -----------
        ; sem block: SignalSemaphore(46) + MinList(12) + ver(2) + rev(2)
        ;            + inline name — AllocMem(128,MEMF_PUBLIC|MEMF_CLEAR)
        movea.l exec_base,a6
        move.l  #128,d0
        move.l  #MEMF_PUBLIC|MEMF_CLEAR,d1
        jsr     LV_AllocMem(a6)
        move.l  d0,sem_ptr
        bne.s   .have_sem
        lea     s_f_t2a(pc),a1
        bra     do_fail
.have_sem:
        movea.l d0,a2
        ; inline name at +62: "SEMPTEST\0"
        lea     sem_name(pc),a3
        lea     62(a2),a4
.copyname:
        move.b  (a3)+,(a4)+
        bne.s   .copyname
        ; Node fields: type NT_SIGNALSEM=15, pri 0, name -> +62
        move.b  #15,8(a2)
        clr.b   9(a2)
        lea     62(a2),a3
        move.l  a3,10(a2)
        ; InitSemaphore(sem=a0)
        move.l  a2,a0
        jsr     LV_InitSemaphore(a6)
        ; private MinList at +46 — empty then one node
        lea     46(a2),a3
        move.l  a3,d1
        addq.l  #4,d1
        move.l  d1,(a3)               ; head -> &tail
        clr.l   4(a3)                 ; tail = 0
        move.l  a3,8(a3)              ; tailpred -> &head
        ; version/revision extension fields (SetPatch layout)
        move.w  #43,58(a2)
        move.w  #6,60(a2)
        ; private node: MinNode(8) + BSTR "PATCH1"
        move.l  #16,d0
        move.l  #MEMF_PUBLIC|MEMF_CLEAR,d1
        jsr     LV_AllocMem(a6)
        move.l  d0,node_ptr
        bne.s   .have_node
        lea     s_f_t2b(pc),a1
        bra     do_fail
.have_node:
        movea.l d0,a3
        move.b  #6,8(a3)              ; BSTR length
        lea     node_name(pc),a4
        lea     9(a3),a5
.copynode:
        move.b  (a4)+,(a5)+
        bne.s   .copynode
        ; link node into the private MinList at sem+46
        lea     46(a2),a4
        move.l  a4,d1
        addq.l  #4,d1                 ; &hdr.tail
        move.l  d1,(a3)               ; node.succ = &tail
        move.l  a4,4(a3)              ; node.pred = &hdr
        move.l  a3,(a4)               ; hdr.head = node
        move.l  a3,8(a4)              ; hdr.tailpred = node
        ; AddSemaphore(sem=a1)
        movea.l sem_ptr,a1
        jsr     LV_AddSemaphore(a6)
        ; same-window sanity: FindSemaphore must see it locally
        lea     sem_name(pc),a1
        jsr     LV_FindSemaphore(a6)
        tst.l   d0
        bne.s   .t2_ok
        lea     s_f_t2c(pc),a1
        bra     do_fail
.t2_ok:
        lea     s_t2_ok(pc),a1
        bsr     print
        ; DO NOT RemSemaphore — the sem persists for SemPatchRead

; ---- RA: ReadArgs parses "QUIET LEVEL=5" -------------------------------
        movea.l dos_base,a6
        lea     ra_template(pc),a0
        move.l  a0,d1
        lea     ra_array(pc),a0
        move.l  a0,d2
        moveq   #0,d3
        jsr     LVO_ReadArgs(a6)
        move.l  d0,ra_handle
        bne.s   .ra_got
        lea     s_f_ra1(pc),a1
        bra     do_fail
.ra_got:
        cmp.l   #-1,ra_array           ; QUIET/S present
        bne.s   .ra_fail
        cmp.l   #5,ra_array+4          ; LEVEL/N == 5
        bne.s   .ra_fail
        move.l  ra_handle,d1
        jsr     LVO_FreeArgs(a6)
        lea     s_ra_ok(pc),a1
        bsr     print
        bra     .pf
.ra_fail:
        lea     s_f_ra2(pc),a1
        bra     do_fail
.pf:

; ---- PF: PrintFault prints the IoErr text -------------------------------
        movea.l dos_base,a6
        move.l  #205,d1                ; ERROR_OBJECT_NOT_FOUND
        lea     pf_hdr(pc),a0
        move.l  a0,d2
        jsr     LVO_PrintFault(a6)
        cmp.l   #-1,d0                 ; DOSTRUE
        beq.s   .pf_ok
        lea     s_f_pf(pc),a1
        bra     do_fail
.pf_ok:
        lea     s_pf_ok(pc),a1
        bsr     print

; ---- RV: MEMF_REVERSE carves the top of the pool -----------------------
        movea.l exec_base,a6
        move.l  #$1000,d0
        move.l  #MEMF_FAST|MEMF_PUBLIC,d1
        jsr     LV_AllocMem(a6)
        move.l  d0,a3
        bne.s   .rv_a
        lea     s_f_rv(pc),a1
        bra     do_fail
.rv_a:
        move.l  #$1000,d0
        move.l  #MEMF_FAST|MEMF_PUBLIC|MEMF_REVERSE,d1
        jsr     LV_AllocMem(a6)
        move.l  d0,a4
        bne.s   .rv_b
        lea     s_f_rv(pc),a1
        bra     do_fail
.rv_b:
        cmp.l   a3,a4                  ; reverse alloc must sit higher
        bls.s   .rv_fail
        lea     s_rv_ok(pc),a1
        bsr     print
        bra     .done
.rv_fail:
        lea     s_f_rv2(pc),a1
        bra     do_fail
.done:
        lea     s_done(pc),a1
        bsr     print
; fall through to prog_exit

prog_exit:
        ; exit via dos Exit — return code in d1
        movea.l dos_base,a6
        moveq   #0,d1
        jsr     -144(a6)               ; dos Exit
        rts

; ---- wrapper installed over FindTask -----------------------------------
my_findtask:
        addq.l  #1,wrap_count
        move.l  a0,-(sp)
        movea.l old_findtask,a0
        jsr     (a0)                   ; old vector -> native FindTask
        move.l  (sp)+,a0
        tst.l   d0
        sne     old_ok                 ; nonzero -> native impl ran
        rts

; ---- helpers -------------------------------------------------------------
print:                                   ; a1 = C string via exec RawPutChar
        movem.l d0/a6,-(sp)
        movea.l 4.w,a6
.p_loop:
        moveq   #0,d0
        move.b  (a1)+,d0
        beq.s   .p_done
        jsr     -516(a6)               ; RawPutChar -> serial
        bra.s   .p_loop
.p_done:
        movem.l (sp)+,d0/a6
        rts

do_fail:                                 ; a1 = C string (FAIL msg)
        bsr     print
        bra     prog_exit

; ---- data (kept in the code section — amigahunk rejects cross-section
; R_PC relocations) ---------------------------------------------------------
        even
exec_base:    dc.l 0
dos_base:     dc.l 0
old_findtask: dc.l 0
wrap_count:   dc.l 0
old_ok:       dc.b 0
              even
sem_ptr:      dc.l 0
node_ptr:     dc.l 0
ra_handle:    dc.l 0
ra_array:     dc.l 0,0,0,0

dos_name:     dc.b "dos.library",0
sem_name:     dc.b "SEMPTEST",0
node_name:    dc.b "PATCH1",0
ra_template:  dc.b "QUIET/S,LEVEL/N",0
pf_hdr:       dc.b "PFHDR",0
s_nodos:      dc.b "SEMPATCH FAIL no dos",10,0
s_t1_ok:      dc.b "SEMPATCH T1 PASS",10,0
s_f_t1a:      dc.b "SEMPATCH T1 FAIL oldvec not trampoline",10,0
s_f_t1b:      dc.b "SEMPATCH T1 FAIL wrapper/oldvec broken",10,0
s_t2_ok:      dc.b "SEMPATCH T2A PASS",10,0
s_f_t2a:      dc.b "SEMPATCH T2 FAIL sem alloc",10,0
s_f_t2b:      dc.b "SEMPATCH T2 FAIL node alloc",10,0
s_f_t2c:      dc.b "SEMPATCH T2 FAIL local FindSemaphore",10,0
s_ra_ok:      dc.b "SEMPATCH RA PASS",10,0
s_f_ra1:      dc.b "SEMPATCH RA FAIL ReadArgs=0",10,0
s_f_ra2:      dc.b "SEMPATCH RA FAIL slots wrong",10,0
s_f_pf:       dc.b "SEMPATCH PF FAIL",10,0
s_pf_ok:      dc.b "SEMPATCH PF PASS",10,0
s_f_rv:       dc.b "SEMPATCH RV FAIL alloc",10,0
s_f_rv2:      dc.b "SEMPATCH RV FAIL order",10,0
s_rv_ok:      dc.b "SEMPATCH RV PASS",10,0
s_done:       dc.b "SEMPATCH DONE",10,0
        end
