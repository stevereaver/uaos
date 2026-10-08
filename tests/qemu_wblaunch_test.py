#!/usr/bin/env python3
"""qemu_wblaunch_test.py — UAOS-253 acceptance: Workbench launch semantics.

Boots UAOS and drives two launches through the wbrun shell command (the
same ExecFile_RunWB path the desktop/filebrowser double-click uses):

  Tool launch    `wbrun SYS:Demos/WBLaunchTest` — the M68k guest must see
                 pr_CLI == 0, receive a WBStartup on pr_MsgPort, find its
                 tooltypes via icon.library (TESTKEY=VALUE42 in
                 WBLaunchTest.info), MatchToolValue, PutDiskObject to
                 RAM:WBPUT.info, ReplyMsg, and exit.
                 Oracles (serial): "WBLAUNCH PASS" + "[wb] startup
                 message replied".

  Project launch `wbrun SYS:Demos/WBProj` — the project icon's
                 do_DefaultTool resolves to SYS:Demos/WBLaunchTest and the
                 project becomes WBArg[1], so sm_NumArgs must be 2.

Requires: QEMU, gdb, built ISO, build/uaos_disk.qcow2.
Run: tests/qemu_wblaunch_test.py [--iso PATH] [--keep] [--timeout SEC]
Exit 0 = all checks passed.
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_layout_test import Fail, Uaos, Monitor, Gdb, launch, \
    wait_serial, REPO


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--iso", default=os.path.join(REPO, "build",
                                                  "Ultimate_Amiga_OS.iso"))
    ap.add_argument("--gdb-port", type=int, default=11246)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--shot-dir", default="/tmp")
    a = ap.parse_args()

    mon = "/tmp/uaos_wbl_mon"
    serial = "/tmp/uaos_wbl_serial.log"
    vars_fd = "/tmp/uaos_wbl_ovmf.fd"
    checks, failed = [], []

    def check(name, ok, detail=""):
        tag = "PASS" if ok else "FAIL"
        checks.append((name, ok))
        print(f"[{tag}] {name}" + (f" — {detail}" if detail else ""),
              flush=True)
        if not ok:
            failed.append(name)

    proc = launch(a.iso, a.gdb_port, mon, serial, vars_fd)
    uaos = Uaos(Monitor(mon), Gdb(a.gdb_port))
    shot = os.path.join(a.shot_dir, "uaos_wblaunch_test.ppm")

    try:
        check("boot: telnetd up",
              wait_serial(serial, "telnetd: listening", 120))

        # ---- tool icon launch --------------------------------------
        uaos.mon.type_string("wbrun SYS:Demos/WBLaunchTest")
        uaos.mon.send("sendkey ret")
        check("tool: guest printed WBLAUNCH PASS",
              wait_serial(serial, "WBLAUNCH PASS", 60))
        check("tool: pr_CLI=0 detected",
              "pr_CLI = 0 (Workbench launch)" in
              open(serial, errors="replace").read())
        check("tool: WBStartup replied on exit",
              wait_serial(serial, "startup message replied", 15))

        # ---- project icon launch (default tool + WBArg[1]) ---------
        # Guest PutStr output interleaves with serial [trace]/->d0=
        # lines; strip those before matching the multi-part print.
        def clean_log():
            log = open(serial, errors="replace").read()
            log = re.sub(r" *\->d0=[^\n]*", "", log)
            log = re.sub(r"\[trace\][^\n]*\n", "", log)
            return log

        uaos.mon.type_string("wbrun SYS:Demos/WBProj")
        uaos.mon.send("sendkey ret")
        ok = False
        t0 = time.time()
        while time.time() - t0 < 60:
            if re.search(r"sm_NumArgs =\s*2", clean_log()):
                ok = True
                break
            time.sleep(1)
        check("project: tool launched, sm_NumArgs=2", ok)
        check("project: second WBLAUNCH PASS",
              open(serial, errors="replace").read()
              .count("WBLAUNCH PASS") >= 2)

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
