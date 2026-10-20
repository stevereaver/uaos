; SemPatchRead.s — UAOS-365 acceptance: cross-window named-semaphore
; materialisation.  SemPatchTest left "SEMPTEST" installed (with the
; SetPatch-style private extension); this second launch runs in a fresh
; guest-RAM window, so FindSemaphore must resolve it through the
; cross-window registry and return a local replica:
;   * sem node type NT_SIGNALSEM
;   * ln_Name "SEMPTEST"
;   * version 43 / revision 6 at +58/+60 (private SetPatch layout)
;   * private MinList at +46 holding one node with BSTR "PATCH1"
;
; Prints "SEMPATCH T2B PASS" or a stage FAIL line.

LVO_OpenLibrary  equ -552
LVO_PutStr       equ -948
LV_FindSemaphore equ -594

        section code,code

start:
        movea.l 4.w,a6
        move.l  a6,exec_base
        lea     dos_name(pc),a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,dos_base
        bne.s   .have_dos
        rts
.have_dos:

        ; FindSemaphore("SEMPTEST")
        movea.l exec_base,a6
        lea     sem_name(pc),a1
        jsr     LV_FindSemaphore(a6)
        move.l  d0,a2
        bne.s   .found
        lea     s_f_find(pc),a1
        bra     do_fail
.found:
        ; NT_SIGNALSEM (15)
        cmp.b   #15,8(a2)
        beq.s   .type_ok
        lea     s_f_type(pc),a1
        bra     do_fail
.type_ok:
        ; ln_Name -> "SEMPTEST"
        movea.l 10(a2),a3
        lea     sem_name(pc),a4
.cmpname:
        move.b  (a4)+,d0
        cmp.b   (a3)+,d0
        bne.s   .name_bad
        tst.b   d0
        bne.s   .cmpname
        bra.s   .ver_ok
.name_bad:
        lea     s_f_name(pc),a1
        bra     do_fail
.ver_ok:
        ; version 43 / revision 6 (SetPatch private layout)
        cmp.w   #43,58(a2)
        bne.s   .ver_bad
        cmp.w   #6,60(a2)
        bne.s   .ver_bad
        bra.s   .list_chk
.ver_bad:
        lea     s_f_ver(pc),a1
        bra     do_fail
.list_chk:
        ; private MinList at +46 — one node whose BSTR is "PATCH1"
        move.l  46(a2),d1              ; mlh_Head -> node or &tail
        beq.s   .list_bad
        lea     46(a2),a3
        addq.l  #4,a3                  ; &hdr.tail
        cmp.l   a3,d1
        beq.s   .list_bad              ; empty (head == &tail)
        movea.l d1,a3                  ; node
        move.l  (a3),d2                ; node.succ must be &hdr.tail
        lea     46(a2),a4
        addq.l  #4,a4
        cmp.l   a4,d2
        bne.s   .list_bad
        ; BSTR at node+8: len 6 + "PATCH1"
        cmp.b   #6,8(a3)
        bne.s   .list_bad
        lea     node_name(pc),a4
        lea     9(a3),a5
.cmpnode:
        move.b  (a4)+,d0
        cmp.b   (a5)+,d0
        bne.s   .list_bad
        tst.b   d0
        bne.s   .cmpnode
        bra.s   .pass
.list_bad:
        lea     s_f_list(pc),a1
        bra     do_fail
.pass:
        lea     s_pass(pc),a1
        bra     print_exit

do_fail:                                 ; a1 = C string
print_exit:                              ; a1 = C string via exec RawPutChar
        movea.l 4.w,a6
.p_loop:
        moveq   #0,d0
        move.b  (a1)+,d0
        beq.s   .p_done
        jsr     -516(a6)               ; RawPutChar -> serial
        bra.s   .p_loop
.p_done:
        movea.l dos_base,a6
        moveq   #0,d1
        jsr     -144(a6)               ; dos Exit
        rts

        even
exec_base:    dc.l 0
dos_base:     dc.l 0
dos_name:     dc.b "dos.library",0
sem_name:     dc.b "SEMPTEST",0
node_name:    dc.b "PATCH1",0
s_f_find:     dc.b "SEMPATCH T2B FAIL not found",10,0
s_f_type:     dc.b "SEMPATCH T2B FAIL node type",10,0
s_f_name:     dc.b "SEMPATCH T2B FAIL name",10,0
s_f_ver:      dc.b "SEMPATCH T2B FAIL version",10,0
s_f_list:     dc.b "SEMPATCH T2B FAIL private list",10,0
s_pass:       dc.b "SEMPATCH T2B PASS",10,0
        end
