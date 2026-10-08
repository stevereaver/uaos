; OptLibsTest.s — UAOS-249 acceptance: optional libraries absent/stubbed.
;
; OctaMED's optional support libraries must either work or fail
; OpenLibrary/OpenDevice cleanly so the feature disables gracefully:
;   amigaguide.library  -> NULL (viewer is native-only)
;   powerpacker.library -> NULL (PP compression not implemented)
;   lh.library          -> NULL (third-party SFCD)
;   rexxsyslib.library  -> NULL (never opened by OctaMED V5.04)
;   diskfont.library    -> opens (ROM stub), all font ops report empty
;   serial.device       -> OpenDevice fails IOERR_OPENFAIL
;   "libs:" path prefix must still decline (basename match)
;
; Prints "OPTLIBS PASS" or "OPTLIBS FAIL <stage>" on stdout/serial.

LVO_OpenLibrary  equ -552
LVO_OpenDevice   equ -444
LVO_PutStr       equ -948
LV_RawPutChar    equ -516

; diskfont.library LVOs
DF_OpenDiskFont  equ -30
DF_AvailFonts    equ -36

IO_DEVICE        equ 20
IO_ERROR         equ 31
IOSTD_SIZE       equ 32

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

; ---- amigaguide.library v34 must fail --------------------------------
        movea.l exec_base,a6
        lea     ag_name(pc),a1
        moveq   #34,d0
        jsr     LVO_OpenLibrary(a6)
        tst.l   d0
        beq.s   .ag_ok
        lea     s_fag(pc),a1
        bra     do_fail
.ag_ok:

; ---- "libs:amigaguide.library" must also fail (basename match) -------
        movea.l exec_base,a6
        lea     ag_path(pc),a1
        moveq   #34,d0
        jsr     LVO_OpenLibrary(a6)
        tst.l   d0
        beq.s   .agp_ok
        lea     s_fagp(pc),a1
        bra     do_fail
.agp_ok:

; ---- powerpacker.library must fail -----------------------------------
        movea.l exec_base,a6
        lea     pp_name(pc),a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        tst.l   d0
        beq.s   .pp_ok
        lea     s_fpp(pc),a1
        bra     do_fail
.pp_ok:

; ---- lh.library must fail --------------------------------------------
        movea.l exec_base,a6
        lea     lh_name(pc),a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        tst.l   d0
        beq.s   .lh_ok
        lea     s_flh(pc),a1
        bra     do_fail
.lh_ok:

; ---- rexxsyslib.library must fail ------------------------------------
        movea.l exec_base,a6
        lea     rx_name(pc),a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        tst.l   d0
        beq.s   .rx_ok
        lea     s_frx(pc),a1
        bra     do_fail
.rx_ok:

; ---- diskfont.library must OPEN (ROM stub) ---------------------------
        movea.l exec_base,a6
        lea     df_name(pc),a1
        moveq   #37,d0
        jsr     LVO_OpenLibrary(a6)
        tst.l   d0
        bne.s   .df_ok
        lea     s_fdf(pc),a1
        bra     do_fail
.df_ok:
        move.l  d0,a4                   ; a4 = diskfont base

; ---- OpenDiskFont(-30) on the stub -> NULL ---------------------------
        movea.l a4,a6
        lea     textattr(pc),a0
        jsr     DF_OpenDiskFont(a6)
        tst.l   d0
        beq.s   .odf_ok
        lea     s_fodf(pc),a1
        bra     do_fail
.odf_ok:

; ---- AvailFonts(-36): returns 0, afh_NumEntries == 0 -----------------
        movea.l a4,a6
        lea     availbuf(pc),a0
        moveq   #64,d0
        moveq   #0,d1
        jsr     DF_AvailFonts(a6)
        tst.l   d0
        beq.s   .af_ok
        lea     s_faf(pc),a1
        bra     do_fail
.af_ok:
        tst.w   availbuf                ; afh_NumEntries must be 0
        beq.s   .af0_ok
        lea     s_faf0(pc),a1
        bra     do_fail
.af0_ok:

; ---- OpenDevice("serial.device") must fail with io_Error != 0 --------
        movea.l exec_base,a6
        lea     ser_name(pc),a0
        moveq   #0,d0                   ; unit 0
        lea     ioreq(pc),a1
        moveq   #0,d1                   ; flags
        jsr     LVO_OpenDevice(a6)
        tst.l   d0
        bne.s   .sd_err                 ; nonzero return = failed (good)
        lea     s_fsd(pc),a1
        bra     do_fail
.sd_err:
        tst.b   ioreq+IO_ERROR          ; io_Error must also be set
        bne.s   .sd_ok
        lea     s_fsde(pc),a1
        bra     do_fail
.sd_ok:

; ---- sanity: utility.library still opens (unaffected) ----------------
        movea.l exec_base,a6
        lea     ut_name(pc),a1
        moveq   #0,d0
        jsr     LVO_OpenLibrary(a6)
        tst.l   d0
        bne.s   .ut_ok
        lea     s_fut(pc),a1
        bra     do_fail
.ut_ok:

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

dos_name:   dc.b "dos.library",0
ag_name:    dc.b "amigaguide.library",0
ag_path:    dc.b "libs:amigaguide.library",0
pp_name:    dc.b "powerpacker.library",0
lh_name:    dc.b "lh.library",0
rx_name:    dc.b "rexxsyslib.library",0
df_name:    dc.b "diskfont.library",0
ut_name:    dc.b "utility.library",0
ser_name:   dc.b "serial.device",0
font_name:  dc.b "ruby.font",0

s_start:    dc.b "OPTLIBS begin",10,0
s_pass:     dc.b "OPTLIBS PASS",10,0

s_fag:      dc.b "OPTLIBS FAIL amigaguide opened",10,0
s_fagp:     dc.b "OPTLIBS FAIL libs:amigaguide opened",10,0
s_fpp:      dc.b "OPTLIBS FAIL powerpacker opened",10,0
s_flh:      dc.b "OPTLIBS FAIL lh opened",10,0
s_frx:      dc.b "OPTLIBS FAIL rexxsyslib opened",10,0
s_fdf:      dc.b "OPTLIBS FAIL diskfont did not open",10,0
s_fodf:     dc.b "OPTLIBS FAIL OpenDiskFont non-NULL",10,0
s_faf:      dc.b "OPTLIBS FAIL AvailFonts error",10,0
s_faf0:     dc.b "OPTLIBS FAIL AvailFonts count!=0",10,0
s_fsd:      dc.b "OPTLIBS FAIL serial.device opened",10,0
s_fsde:     dc.b "OPTLIBS FAIL serial io_Error==0",10,0
s_fut:      dc.b "OPTLIBS FAIL utility did not open",10,0

        even
textattr:   dc.l font_name              ; ta_Name
            dc.w 9                      ; ta_YSize
            dc.b 0,0                    ; ta_Style, ta_Flags

        even
ioreq:      ds.b IOSTD_SIZE
availbuf:   ds.b 64

        end
