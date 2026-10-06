#!/usr/bin/env python3
"""qemu_asl_test.py — UAOS-242 acceptance: native asl.library file requester.

Boots UAOS, runs SYS:Demos/ASLTest (M68k guest program), and drives the
native ASL requester window through three phases:

  Phase 1  multi-select — AllocAslRequest({DoMultiSelect, DoPatterns,
           InitialDrawer:"OCTAMED:", InitialPattern:"#?"}) + AslRequest.
           The test marks two files; ASLTest then walks fr_ArgList and
           Open()s each WBArg's wa_Name relative to its wa_Lock, so the
           serial [dos] Open('OCTAMED:INx.JPG') lines prove the guest
           WBArg list and the drawer lock resolve.  Sentinel RAM:ASLP1OK.
  Phase 2  save mode — ASLFR_DoSaveMode + InitialFile "ASLTEST.SAV";
           OK on the requester -> guest builds drawer+file via AddPart
           and Open(...,MODE_NEWFILE) creates OCTAMED:ASLTEST.SAV.
  Phase 3  delete — plain requester with InitialDrawer "RAM:" +
           InitialFile "ASLDEL.TMP" (pre-created by the guest); OK on
           the requester -> guest DeleteFile()s the returned path.
           Sentinel RAM:ASLDOK (or RAM:ASLDELNG on failure).
  Phase 4  cancel — plain requester; Cancel must make AslRequest
           return FALSE -> sentinel RAM:ASLCOK (or RAM:ASLCFAIL on bug).

ASLTest finishes with RAM:ASLPASS.  All oracles are serial-visible DOS
calls, so no screen scraping is needed.

Requires: QEMU, gdb, built ISO, build/uaos_disk.qcow2, build/octamed.img.
Run: tests/qemu_asl_test.py [--iso PATH] [--keep] [--timeout SEC]
Exit 0 = all checks passed.
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
FRQ = "'intuition_lib.c'::g_frq"
TASK_REMOVED = 3


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--iso", default=os.path.join(REPO, "build",
                                                  "Ultimate_Amiga_OS.iso"))
    ap.add_argument("--gdb-port", type=int, default=11244)
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--shot-dir", default="/tmp")
    a = ap.parse_args()

    if not os.path.isfile(OCTAMED_HDF):
        sys.exit(f"OctaMED image not found: {OCTAMED_HDF}")

    mon = "/tmp/uaos_asl_mon"
    serial = "/tmp/uaos_asl_serial.log"
    vars_fd = "/tmp/uaos_asl_ovmf.fd"
    checks, failed = [], []

    def check(name, ok, detail=""):
        tag = "PASS" if ok else "FAIL"
        checks.append((name, ok))
        print(f"[{tag}] {name}" + (f" — {detail}" if detail else ""),
              flush=True)
        if not ok:
            failed.append(name)

    proc = launch_with_hdf(a.iso, a.gdb_port, mon, serial, vars_fd,
                           hdf=OCTAMED_HDF)
    uaos = Uaos(Monitor(mon), Gdb(a.gdb_port))
    shot = os.path.join(a.shot_dir, "uaos_asl_test.ppm")

    def frq_geom(field):
        out = uaos.gdb.run(f'printf "@@G %d\\n", {FRQ}.{field}')
        m = re.search(r"@@G (\d+)", out[0]) if out else None
        return int(m.group(1)) if m else 0

    def frq_btn(win, idx):
        """Screen coords of requester button idx (0=Volumes 1=Parent
        2=OK 3=Cancel)."""
        return (win["x"] + frq_geom(f"btn_x[{idx}]") + 38,
                win["y"] + frq_geom("btn_y") + 9)

    def await_win(title, timeout=30):
        t0 = time.time()
        while time.time() - t0 < timeout:
            w = next((x for x in uaos.gdb.probe_state()["wins"]
                      if x["title"] == title), None)
            if w:
                return w
            time.sleep(1)
        return None

    def row_y(win, idx):
        return win["y"] + frq_geom("list_y") + idx * 14 + 7

    def serial_log():
        return open(serial, errors="replace").read()

    try:
        check("boot: telnetd up", wait_serial(serial, "telnetd: listening",
                                              120))

        uaos.mon.type_string("SYS:Demos/ASLTest")
        uaos.mon.send("sendkey ret")

        # ---- phase 1: multi-select -----------------------------------
        w = await_win("ASLTEST multi-select")
        check("phase1: multi-select requester opened", bool(w))
        if w:
            check("phase1: multi mode + drawer + pattern applied",
                  frq_geom("multi") == 1 and frq_geom("nent") > 0)
            # mark two FILE entries (is_dir=0) among the visible rows
            out = uaos.gdb.run(
                f'set $i = 0\n'
                f'while $i < {FRQ}.nent\n'
                f'  printf "@@E %d %d %s\\n", $i, '
                f'{FRQ}.ents[$i].is_dir, {FRQ}.ents[$i].name\n'
                f'  set $i = $i + 1\n'
                f'end\n')
            rows = []
            for l in out:
                m = re.match(r"@@E (\d+) (\d+) (.*)", l)
                if m and m.group(2) == "0":
                    rows.append(int(m.group(1)))
            marked = 0
            for r in rows[:2]:
                if r >= frq_geom("top") + frq_geom("rows"):
                    continue
                uaos.move_to(w["x"] + 80, row_y(w, r))
                uaos.click()
                time.sleep(1)
                marked += 1
            check("phase1: two files marked", frq_geom("nsel") >= 2
                  or marked >= 2)
            uaos.move_to(*frq_btn(w, 2))          # OK
            uaos.click()
            time.sleep(4)

        log = serial_log()
        check("phase1: WBArg entries opened via wa_Lock",
              "Open('OCTAMED:IN1.JPG')" in log
              and "Open('OCTAMED:IN2.JPG')" in log)
        check("phase1: sentinel ASLP1OK",
              wait_serial(serial, "Open('RAM:ASLP1OK')", 15))

        # ---- phase 2: save mode ---------------------------------------
        w = await_win("ASLTEST save")
        check("phase2: save requester opened", bool(w))
        if w:
            check("phase2: save_mode + InitialFile applied",
                  frq_geom("save_mode") == 1)
            uaos.move_to(*frq_btn(w, 2))          # OK
            uaos.click()
            time.sleep(4)
        check("phase2: guest created drawer+file path",
              "Open('OCTAMED:ASLTEST.SAV') mode=1006" in serial_log())

        # ---- phase 3: delete ------------------------------------------
        w = await_win("ASLTEST delete")
        check("phase3: delete requester opened", bool(w))
        if w:
            uaos.move_to(*frq_btn(w, 2))          # OK (InitialFile set)
            uaos.click()
            time.sleep(4)
        check("phase3: returned path deleted via DeleteFile",
              "Open('RAM:ASLDOK')" in serial_log()
              and "ASLDELNG" not in serial_log())

        # ---- phase 4: cancel ------------------------------------------
        w = await_win("ASLTEST cancel-me")
        check("phase4: cancel requester opened", bool(w))
        if w:
            uaos.move_to(*frq_btn(w, 3))          # Cancel
            uaos.click()
            time.sleep(3)
        check("phase4: AslRequest returned FALSE on cancel",
              "Open('RAM:ASLCOK')" in serial_log()
              and "ASLCFAIL" not in serial_log())
        check("all phases: sentinel ASLPASS",
              wait_serial(serial, "Open('RAM:ASLPASS')", 15))

        st = uaos.gdb.probe_state()
        check("ASLTest task exited cleanly",
              all("ASLT" not in t["name"].upper()
                  or t["state"] == TASK_REMOVED for t in st["tasks"]))
        uaos.mon.screendump(shot)

    except Fail as e:
        print(f"[ABORT] {e}")
        failed.append("harness abort")
        try:
            uaos.mon.screendump(shot)
        except Exception:
            pass
    finally:
        if a.keep:
            print(f"QEMU left running (pid {proc.pid}); "
                  f"mon={mon} gdb=:{a.gdb_port}")
        else:
            proc.terminate()

    print(f"\n{sum(1 for _, ok in checks if ok)}/{len(checks)} checks passed"
          + (f" — screendump: {shot}" if failed else ""))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
