; SpinTest.s — UAOS-247 regression: CPU-spinning guest + liveness watchdog
;
; Burns M68k cycles in a tight loop and NEVER calls into a library, so it
; never enters a blocking nap: spin_cycles grows until the 100M-cycle
; liveness watchdog fires (once, non-fatal — the guest keeps running).
; Task teardown must still work when the task is externally halted
; (m68k_halted) or removed.
;
; Assemble: vasmm68k_mot -Fhunk -o SpinTest.o SpinTest.s
; Link:     vlink -bamigahunk -o SpinTest.hunk SpinTest.o
;

        section code,code

start:
        moveq   #0,d0
        moveq   #0,d1
        moveq   #0,d2
        moveq   #0,d3
loop:
        addq.l  #1,d0
        subq.l  #1,d1
        eor.l   d2,d3
        rol.l   #3,d3
        bra.s   loop
