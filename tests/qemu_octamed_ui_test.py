#!/usr/bin/env python3
"""qemu_octamed_ui_test.py — OctaMED UI rendering repro/driver.

Boots UAOS with build/octamed.img, launches OctaMED V5, dismisses the
startup requesters, waits for the tracker UI, then:

  * takes a QEMU screendump (PPM -> PNG if PIL present)
  * probes the OctaMED m68k task's screen/window RastPort font fields
    through the GDB stub (TextFont: YSize/XSize/Flags/LoChar/HiChar/
    CharData/Modulo/CharLoc/CharSpace/CharKern) so the glyph path in
    graphics_lib.c::draw_text_char can be verified against reality.

Run: tests/qemu_octamed_ui_test.py [--keep] [--shot-dir DIR]
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_layout_test import Fail, Gdb, Monitor, Uaos, wait_serial, REPO
from qemu_m68k_lifecycle_test import launch_with_hdf

OCTAMED_HDF = os.path.join(REPO, "build", "octamed.img")
TASK_REMOVED = 3

# guest TextFont offsets (amiga_graphics.h — real AmigaOS layout)
TF = {"ys": 20, "style": 22, "flags": 23, "xs": 24, "base": 26,
      "lo": 32, "hi": 33, "data": 34, "mod": 38, "loc": 40,
      "space": 44, "kern": 48}


def be16(ram, off):
    return ram[off] * 256 + ram[off + 1]


def be32(ram, off):
    return ram[off] * 16777216 + ram[off + 1] * 65536 + \
        ram[off + 2] * 256 + ram[off + 3]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--iso", default=os.path.join(REPO, "build",
                                                  "Ultimate_Amiga_OS.iso"))
    ap.add_argument("--gdb-port", type=int, default=11240)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--shot-dir", default="/tmp")
    a = ap.parse_args()

    mon = "/tmp/uaos_ouitest_mon"
    serial = "/tmp/uaos_ouitest_serial.log"
    vars_fd = "/tmp/uaos_ouitest_ovmf.fd"

    proc = launch_with_hdf(a.iso, a.gdb_port, mon, serial, vars_fd,
                           hdf=OCTAMED_HDF)
    uaos = Uaos(Monitor(mon), Gdb(a.gdb_port))
    shot = os.path.join(a.shot_dir, "uaos_octamed_ui.ppm")

    def probe():
        return uaos.gdb.probe_state()

    def dismiss_request():
        st = probe()
        req = next((w for w in st["wins"] if w["title"] == "Request"), None)
        if req:
            print("dismiss:", req)
            # log reports button0 at rel (65,50): click there
            uaos.move_to(req["x"] + 65, req["y"] + 50)
            uaos.click()
            time.sleep(2)
            return True
        return False

    try:
        if not wait_serial(serial, "telnetd: listening", 120):
            raise Fail("boot never reached telnetd")

        uaos.mon.type_string("run OCTAMED:OCTAMED/OCTAMED.V5")
        uaos.mon.send("sendkey ret")
        octa = None
        t0 = time.time()
        while time.time() - t0 < 120 and not octa:
            st = probe()
            octa = next((t for t in st["tasks"]
                         if "OCTAMED" in t["name"].upper()
                         and t["state"] != TASK_REMOVED), None)
            req = next((w for w in st["wins"] if w["title"] == "Request"),
                       None)
            if req:
                print("dismiss:", req)
                uaos.move_to(req["x"] + 65, req["y"] + 50)
                uaos.click()
            time.sleep(5)
        if not octa:
            raise Fail("OctaMED task never appeared")
        print("OctaMED task up:", octa)

        # clear startup requesters
        for _ in range(20):
            if not dismiss_request():
                break

        # let the tracker UI settle
        time.sleep(8)
        uaos.mon.screendump(shot)
        print("screendump:", shot)

        # ---- dump the guest TextFont of OctaMED's screen rastport ----
        # Screen->RastPort at guest screen+... find via intuition window's
        # rastport instead: Window->RPort at offset 50 in UAOS layout.
        script = r'''
set $ram = (unsigned char*)RAMADDR
set $i = 0
while $i < 8
  if g_intu_wins[$i].active && g_intu_wins[$i].guest_win != 0
    set $w = $ram + g_intu_wins[$i].guest_win
    set $rp = $w[50]*16777216 + $w[51]*65536 + $w[52]*256 + $w[53]
    printf "@@WIN i=%d gw=0x%x rp=0x%x wscr=0x%x\n", $i, g_intu_wins[$i].guest_win, $rp, $w[46]*16777216+$w[47]*65536+$w[48]*256+$w[49]
    printf "@@WINGEO w=%d h=%d title=0x%x gad=0x%x\n", $w[8]*256+$w[9], $w[10]*256+$w[11], $w[32]*16777216+$w[33]*65536+$w[34]*256+$w[35], $w[62]*16777216+$w[63]*65536+$w[64]*256+$w[65]
    set $tp = $w[32]*16777216+$w[33]*65536+$w[34]*256+$w[35]
    if $tp != 0 && $tp < 0x4000000
      x/28cb $ram + $tp
    end
    set $g = $w[62]*16777216+$w[63]*65536+$w[64]*256+$w[65]
    set $n = 0
    while $g != 0 && $n < 30
      set $gg = $ram + $g
      printf "@@GAD n=%d t=0x%x x=%d y=%d w=%d h=%d it=0x%x\n", $n, $gg[16]*256+$gg[17], $gg[4]*256+$gg[5], $gg[6]*256+$gg[7], $gg[8]*256+$gg[9], $gg[10]*256+$gg[11], $gg[26]*16777216+$gg[27]*65536+$gg[28]*256+$gg[29]
      set $it = $gg[26]*16777216+$gg[27]*65536+$gg[28]*256+$gg[29]
      if $it != 0
        set $i2 = $ram + $it
        printf "@@ITX x=%d y=%d attr=0x%x str=0x%x ", $i2[4]*256+$i2[5], $i2[6]*256+$i2[7], $i2[8]*16777216+$i2[9]*65536+$i2[10]*256+$i2[11], $i2[12]*16777216+$i2[13]*65536+$i2[14]*256+$i2[15]
        set $sp = $i2[12]*16777216+$i2[13]*65536+$i2[14]*256+$i2[15]
        if $sp != 0 && $sp < 0x4000000
          x/20cb $ram + $sp
        end
        set $ap = $i2[8]*16777216+$i2[9]*65536+$i2[10]*256+$i2[11]
        if $ap != 0 && $ap < 0x4000000
          set $a2 = $ram + $ap
          set $np = $a2[0]*16777216+$a2[1]*65536+$a2[2]*256+$a2[3]
          printf "@@ATTR ys=%d name=0x%x ", $a2[4]*256+$a2[5], $np
          if $np != 0 && $np < 0x4000000
            x/20cb $ram + $np
          end
        end
      end
      set $g = $gg[0]*16777216+$gg[1]*65536+$gg[2]*256+$gg[3]
      set $n = $n + 1
    end
    if $rp != 0
      set $r = $ram + $rp
      set $font = $r[52]*16777216 + $r[53]*65536 + $r[54]*256 + $r[55]
      printf "@@RP font=0x%x drawmode=%d fg=%d bg=%d cpx=%d cpy=%d bm=0x%x\n", $font, $r[28], $r[25], $r[26], $r[36]*256+$r[37], $r[38]*256+$r[39], $r[4]*16777216+$r[5]*65536+$r[6]*256+$r[7]
      if $font != 0
        set $f = $ram + $font
        printf "@@FONT ys=%d xs=%d style=%d flags=0x%x base=%d lo=%d hi=%d data=0x%x mod=%d loc=0x%x space=0x%x kern=0x%x\n", $f[20]*256+$f[21], $f[24]*256+$f[25], $f[22], $f[23], $f[26]*256+$f[27], $f[32], $f[33], $f[34]*16777216+$f[35]*65536+$f[36]*256+$f[37], $f[38]*256+$f[39], $f[40]*16777216+$f[41]*65536+$f[42]*256+$f[43], $f[44]*16777216+$f[45]*65536+$f[46]*256+$f[47], $f[48]*16777216+$f[49]*65536+$f[50]*256+$f[51]
      end
    end
  end
  set $i = $i + 1
end
set $i = 0
while $i < 4
  if g_intu_screens[$i].active && g_intu_screens[$i].guest_screen != 0
    set $sc = $ram + g_intu_screens[$i].guest_screen
    printf "@@SCR i=%d sc=0x%x w=%d h=%d flags=0x%x\n", $i, g_intu_screens[$i].guest_screen, $sc[12]*256+$sc[13], $sc[14]*256+$sc[15], $sc[20]*256+$sc[21]
    set $rp = $sc + 84
    set $font = $rp[52]*16777216 + $rp[53]*65536 + $rp[54]*256 + $rp[55]
    printf "@@SCRRP bm=0x%x font=0x%x dm=%d fg=%d bg=%d mask=%d\n", $rp[4]*16777216+$rp[5]*65536+$rp[6]*256+$rp[7], $font, $rp[28], $rp[25], $rp[26], $rp[24]
    set $bm = $rp[4]*16777216+$rp[5]*65536+$rp[6]*256+$rp[7]
    if $bm != 0
      set $b = $ram + $bm
      printf "@@SCRRPBM bpr=%d rows=%d depth=%d p0=0x%x\n", $b[0]*256+$b[1], $b[2]*256+$b[3], $b[5], $b[8]*16777216+$b[9]*65536+$b[10]*256+$b[11]
    end
    if $font != 0
      set $f = $ram + $font
      printf "@@SCRFONT ys=%d xs=%d flags=0x%x lo=%d hi=%d data=0x%x mod=%d loc=0x%x\n", $f[20]*256+$f[21], $f[24]*256+$f[25], $f[23], $f[32], $f[33], $f[34]*16777216+$f[35]*65536+$f[36]*256+$f[37], $f[38]*256+$f[39], $f[40]*16777216+$f[41]*65536+$f[42]*256+$f[43]
    end
  end
  set $i = $i + 1
end
'''.replace("RAMADDR", str(octa["ram"]))
        out = uaos.gdb.run(script)
        for l in out:
            print("  ", l)

    except Fail as e:
        print(f"[ABORT] {e}")
        try:
            uaos.mon.screendump(shot)
        except Exception:
            pass
    finally:
        if a.keep:
            print(f"QEMU running pid={proc.pid} mon={mon} gdb=:{a.gdb_port}")
        else:
            proc.terminate()


if __name__ == "__main__":
    main()
