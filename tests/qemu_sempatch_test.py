#!/usr/bin/env python3
"""qemu_sempatch_test.py — UAOS-365 acceptance: SetPatch-driven fixes.

Boots UAOS and drives two guest launches through the shell:

  SYS:Demos/SemPatchTest QUIET LEVEL=5
      T1  SetFunction on an ILLEGAL stub returns a callable old vector —
          a wrapper jsr's it and must reach the native FindTask impl.
      T2A AddSemaphore registers the named sem cross-window (local
          FindSemaphore sanity inside the same launch).
      RA  ReadArgs("QUIET/S,LEVEL/N") against the launch command line.
      PF  PrintFault dispatched + returns DOSTRUE.
      RV  AllocMem(MEMF_FAST|MEMF_REVERSE) returns a higher address than
          a same-size non-reverse fast alloc.
      Leaves the "SEMPTEST" named semaphore installed.

  SYS:Demos/SemPatchRead
      T2B Second launch, fresh guest-RAM window: FindSemaphore("SEMPTEST")
          materialises a local replica carrying the sem type, name,
          version 43/6 fields and the private MinList BSTR node.

Guests report via exec/RawPutChar (-516), decoded from the lib=1 fn=98
strace lines on serial — same as qemu_guesttask_test.py.

Requires: QEMU, gdb, built ISO, build/uaos_disk.qcow2.
Run: tests/qemu_sempatch_test.py [--iso PATH] [--keep] [--timeout SEC]
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

RAWPUTCHAR_RE = re.compile(r"lib=1 fn=98 d0=0x([0-9A-Fa-f]+)")


def decode_rawputchar(log):
    return "".join(chr(int(h, 16) & 0xFF)
                   for h in RAWPUTCHAR_RE.findall(log))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--iso", default=os.path.join(REPO, "build",
                                                  "Ultimate_Amiga_OS.iso"))
    ap.add_argument("--gdb-port", type=int, default=11251)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()

    mon = "/tmp/uaos_semp_mon"
    serial = "/tmp/uaos_semp_serial.log"
    vars_fd = "/tmp/uaos_semp_ovmf.fd"
    checks, failed = [], []

    def check(name, ok, detail=""):
        tag = "PASS" if ok else "FAIL"
        checks.append((name, ok))
        print(f"[{tag}] {name}" + (f" — {detail}" if detail else ""),
              flush=True)
        if not ok:
            failed.append(name)

    def wait_shell(u):
        t0 = time.time()
        while time.time() - t0 < 60:
            try:
                st = u.gdb.probe_state()
                if st and any("Shell" in w["title"] for w in st["wins"]):
                    return True
            except Fail:
                pass
            time.sleep(1.5)
        return False

    def wait_guest(needle, seconds=60):
        t0 = time.time()
        while time.time() - t0 < seconds:
            try:
                log = open(serial, errors="replace").read()
            except FileNotFoundError:
                time.sleep(1)
                continue
            if needle in decode_rawputchar(log):
                return True
            time.sleep(1)
        return False

    proc = launch(a.iso, a.gdb_port, mon, serial, vars_fd)
    uaos = Uaos(Monitor(mon), Gdb(a.gdb_port))

    try:
        check("boot: telnetd up",
              wait_serial(serial, "telnetd: listening", 120))
        check("boot: shell window", wait_shell(uaos))

        # ---- writer + API checks (first window) --------------------
        uaos.mon.type_string("SYS:Demos/SemPatchTest QUIET LEVEL=5")
        uaos.mon.send("sendkey ret")
        for needle in ("SEMPATCH T1 PASS", "SEMPATCH T2A PASS",
                       "SEMPATCH RA PASS", "SEMPATCH PF PASS",
                       "SEMPATCH RV PASS", "SEMPATCH DONE"):
            check(f"SemPatchTest: {needle}", wait_guest(needle))
        guest = decode_rawputchar(open(serial, errors="replace").read())
        check("SemPatchTest: no FAIL line",
              "SEMPATCH" not in guest or "FAIL" not in guest)

        # PrintFault dispatched to the new handler (dos fn 104 in strace)
        check("PrintFault: fn=104 dispatched",
              "lib=2 fn=104" in
              open(serial, errors="replace").read())

        # ---- reader (second window — registry materialisation) -----
        uaos.mon.type_string("SYS:Demos/SemPatchRead")
        uaos.mon.send("sendkey ret")
        check("SemPatchRead: SEMPATCH T2B PASS",
              wait_guest("SEMPATCH T2B PASS"))

    except Fail as e:
        print(f"[ABORT] {e}")
        failed.append("harness abort")
    finally:
        if a.keep:
            print(f"QEMU left running (pid {proc.pid}); "
                  f"mon={mon} gdb=:{a.gdb_port}")
        else:
            proc.terminate()

    print(f"\n{sum(1 for _, ok in checks if ok)}/{len(checks)} checks passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
