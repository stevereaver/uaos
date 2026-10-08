; LeakFile.s — UAOS-247 regression: orphaned DOS file/lock handles on exit
;
; Opens a file (SYS:S/User-Startup, MODE_OLDFILE) and a lock (RAM:,
; ACCESS_READ), then exits WITHOUT Close() / UnLock().  Task teardown
; (HandleTable_FreeByOwner) must sweep the global handle table — a leaked
; file entry leaves a live FileHandle Action-ctx plus VFS refcount, a
; leaked lock pins the handler-side FileLock node forever.
;
; Assemble: vasmm68k_mot -Fhunk -o LeakFile.o LeakFile.s
; Link:     vlink -bamigahunk -o LeakFile.hunk LeakFile.o
;

LVO_OpenLibrary    equ -552
LVO_Open           equ -30
LVO_Lock           equ -84

MODE_OLDFILE       equ 1005
ACCESS_READ        equ -2

        section code,code

start:
        movea.l $4.w,a6
        move.l  #libname_dos,a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        move.l  d0,a6                     ; a6 = dos_base
        beq.w   exit

        move.l  #file_name,d1
        move.l  #MODE_OLDFILE,d2
        jsr     LVO_Open(a6)              ; -> global handle slot (leaked)

        move.l  #dir_name,d1
        move.l  #ACCESS_READ,d2
        jsr     LVO_Lock(a6)              ; -> lock slot (leaked)

        ; Deliberate leak: no Close(), no UnLock(), no CloseLibrary.
exit:
        rts                               ; return address is the DOS Exit stub

        section data,data
libname_dos:  dc.b 'dos.library',0
file_name:    dc.b 'SYS:S/User-Startup',0
dir_name:     dc.b 'RAM:',0
        even
