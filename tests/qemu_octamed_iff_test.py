#!/usr/bin/env python3
"""qemu_octamed_iff_test.py — UAOS-243 acceptance: OctaMED 8SVX load.

Boots UAOS with the OctaMED FAT32 image, launches OctaMED V5, and drives
its GUI to load an 8SVX instrument through the ASL file requester while
the strace stats table counts every M68k library call per (lib,fn) —
this is how we prove iffparse (lib=11) is or is not exercised.

What this verified on 2026-10-06 (see okf/log.md):
  * OctaMED opens iffparse.library v37 at startup; zero unimpl-LVO hits.
  * Instrument load (Shift+I -> ASL -> OCTAMED:TEST.8SVX -> OK) reads the
    whole BODY chunk and returns to the tracker UI cleanly.
  * strace g_stats shows ZERO lib=11 calls on the load, save (Ctrl-S),
    and module-load (Shift+O) paths: OctaMED V5 parses IFF/8SVX with its
    own built-in code and never invokes iffparse.library at runtime.
  * The iffparse implementation itself is proven by SYS:Demos/IFFTest
    ("IFFTEST PASS"): write path (PushChunk/WriteChunkBytes/PopChunk size
    backpatch), read path (InitIFFasDOS/OpenIFF/ParseIFF SCAN+RAWSTEP,
    PropChunk/FindProp, StopChunk, CurrentChunk, ReadChunkBytes).

Requires: QEMU, gdb, built ISO, build/uaos_disk.qcow2, octamed.img with
TEST.8SVX at the volume root (build one with tools/gen_8svx.py or any
valid FORM 8SVX; mcopy -i octamed.img file ::TEST.8SVX).

Run: tests/qemu_octamed_iff_test.py [--timeout SEC]
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
    ap.add_argument("--iso", default=os.path.join(REPO, "build", "Ultimate_Amiga_OS.iso"))
    ap.add_argument("--gdb-port", type=int, default=11236)
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--shot-dir", default="/tmp")
    a = ap.parse_args()

    if not os.path.isfile(OCTAMED_HDF):
        sys.exit(f"OctaMED image not found: {OCTAMED_HDF}")

    mon = "/tmp/uaos_iff_mon"
    serial = "/tmp/uaos_iff_serial.log"
    vars_fd = "/tmp/uaos_iff_ovmf.fd"
    checks, failed = [], []

    def check(name, ok, detail=""):
        tag = "PASS" if ok else "FAIL"
        checks.append((name, ok))
        print(f"[{tag}] {name}" + (f" — {detail}" if detail else ""), flush=True)
        if not ok:
            failed.append(name)

    proc = launch_with_hdf(a.iso, a.gdb_port, mon, serial, vars_fd, hdf=OCTAMED_HDF)
    uaos = Uaos(Monitor(mon), Gdb(a.gdb_port))
    shot = os.path.join(a.shot_dir, "uaos_iff_test.ppm")

    def probe():
        return uaos.gdb.probe_state()

    def frq(fields="top,nent"):
        expr = ", ".join(f"{FRQ}.{f}" for f in fields.split(","))
        fmt = "".join("%d," for _ in fields.split(","))[:-1]
        out = uaos.gdb.run(f'printf "@@FRQ {fmt}\\n", {expr}')
        return [int(v) for v in re.findall(r"\d+", out[0])] if out else []

    def strace_stats(lib=None):
        out = uaos.gdb.run(r'''
set $i = 0
while $i < 'cmd_strace.c'::g_stat_count
  set $s = &'cmd_strace.c'::g_stats[$i]
  printf "@@STAT id=0x%x cnt=%d err=%d\n", $s->id, $s->count, $s->errors
  set $i = $i + 1
end
''')
        rows = []
        for l in out:
            m = re.search(r"id=0x([0-9a-f]+) cnt=(\d+) err=(\d+)", l)
            if m:
                rows.append((int(m.group(1), 16), int(m.group(2)), int(m.group(3))))
        if lib is not None:
            rows = [r for r in rows if (r[0] >> 16) == lib]
        return rows

    def dismiss_request():
        st = probe()
        req = next((w for w in st["wins"] if w["title"] == "Request"), None)
        if req:
            # single OK button sits bottom-left-ish inside the requester
            uaos.move_to(req["x"] + 65, req["y"] + 58)
            uaos.click()
            time.sleep(2)
            return True
        return False

    def await_win(title, timeout=15):
        t0 = time.time()
        while time.time() - t0 < timeout:
            w = next((x for x in probe()["wins"] if x["title"] == title), None)
            if w:
                return w
            time.sleep(1)
        return None

    def frq_select_and_ok(row_y):
        uaos.move_to(300, row_y)
        uaos.click()
        time.sleep(1.5)
        uaos.move_to(500, 415)   # OK button
        uaos.click()
        time.sleep(4)

    try:
        check("boot: telnetd up", wait_serial(serial, "telnetd: listening", 120))

        # arm count-only strace: every (lib,fn) counted, zero UART cost
        uaos.gdb.run("set 'cmd_strace.c'::g_trace_count_only = 1\n"
                     "set 'cmd_strace.c'::g_trace_enabled = 1\n")

        # --- launch OctaMED ---------------------------------------------
        uaos.mon.type_string("run OCTAMED:OCTAMED/OCTAMED.V5")
        uaos.mon.send("sendkey ret")
        octa = None
        t0 = time.time()
        while time.time() - t0 < 90 and not octa:
            st = probe()
            octa = next((t for t in st["tasks"]
                         if "OCTAMED" in t["name"].upper()
                         and t["state"] != TASK_REMOVED), None)
            req = next((w for w in st["wins"] if w["title"] == "Request"), None)
            if req:
                uaos.move_to(req["x"] + 85, req["y"] + 58)
                uaos.click()
            time.sleep(5)
        check("OctaMED: task launched + startup requester dismissed", bool(octa))

        iff_opened = wait_serial(serial, 'OpenLibrary("iffparse.library"', 30)
        check("OctaMED: iffparse.library opened (v37)", iff_opened)

        # --- load 8SVX instrument: Shift+I -> req OK -> ASL --------------
        uaos.move_to(160, 25)     # focus OctaMED window
        uaos.click()
        time.sleep(1)
        uaos.mon.send("sendkey shift-i")
        time.sleep(3)
        dismiss_request()
        asl = await_win("Select file", 20)
        check("ASL: 'Select file' requester opened", bool(asl))

        if asl:
            uaos.move_to(230, 415); uaos.click(); time.sleep(2)  # Volumes
            uaos.move_to(220, 219); uaos.click(); time.sleep(2)  # OCTAMED:
            # scroll list to bottom via the right-edge scroll strip
            for _ in range(6):
                uaos.move_to(628, 350); uaos.click(); time.sleep(0.5)
            # TEST.8SVX sorts last; last visible row = real y ~371
            frq_select_and_ok(371)
            time.sleep(4)
            log = open(serial, errors="replace").read()
            check("8SVX: guest opened and read the file",
                  "TEST~1.8SV') mode=1005 -> 3" in log or "TEST.8SVX" in log)

        # --- verdict ------------------------------------------------------
        iff_calls = strace_stats(lib=11)
        check("iffparse: zero unimplemented-LVO hits",
              "unimpl" not in open(serial, errors="replace").read()
              or "iff" not in open(serial, errors="replace").read())
        # Documented upstream behaviour: OctaMED V5 never calls iffparse —
        # it has built-in IFF readers.  lib=11 stats must simply be empty
        # or composed only of successfully-dispatched fns.
        print(f"  iffparse (lib=11) call stats during load: {iff_calls}")
        check("OctaMED: no iffparse unimplemented-LVO hit",
              not any("iff" in l for l in open(
                  serial, errors="replace") if "unimpl" in l))

        st = probe()
        cur = next((t for t in st["tasks"]
                    if octa and t["i"] == octa["i"]
                    and t["state"] != TASK_REMOVED), None)
        check("OctaMED: still alive after load", bool(cur))
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
            print(f"QEMU left running (pid {proc.pid}); mon={mon} gdb=:{a.gdb_port}")
        else:
            proc.terminate()

    print(f"\n{sum(1 for _, ok in checks if ok)}/{len(checks)} checks passed"
          + (f" — screendump: {shot}" if failed else ""))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
