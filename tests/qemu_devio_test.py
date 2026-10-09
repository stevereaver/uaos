#!/usr/bin/env python3
"""qemu_devio_test.py — UAOS-240 regression: guest device I/O framework.

Boots the UAOS ISO headless, types `SYS:Demos/DevIOTest` into the Shell,
and watches the serial log for the demo's verdict.  The demo exercises:

  * OpenDevice("timer.device") + SendIO(TR_ADDREQUEST): request must pend
    (CheckIO == 0), expiry replies the request onto mn_ReplyPort and
    signals mp_SigTask/mp_SigBit — Wait()+GetMsg() must return it.
  * DoIO(TR_GETSYSTIME) fills tv_secs.
  * OpenDevice("console.device") + CMD_WRITE emits text.
  * OpenDevice("serial.device") must fail (declined-device policy).
  * keyboard.device opens; SendIO(KBD_READEVENT) pends; AbortIO + WaitIO
    return IOERR_ABORTED.

Requires: gdb, QEMU, a built ISO + build/uaos_disk.qcow2.
Run:  tests/qemu_devio_test.py [--iso PATH] [--timeout SEC]

Exit 0 = DEVIOTEST PASS seen on the serial log.
"""

import argparse
import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_layout_test import (Fail, Gdb, Monitor, Uaos, launch, wait_serial,
                              REPO, KERNEL_ELF)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--iso", default=os.path.join(REPO, "build", "Ultimate_Amiga_OS.iso"))
    ap.add_argument("--gdb-port", type=int, default=11240)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--keep", action="store_true", help="leave QEMU running")
    a = ap.parse_args()

    mon = "/tmp/uaos_devio_mon"
    serial = "/tmp/uaos_devio_serial.log"
    vars_fd = "/tmp/uaos_devio_ovmf.fd"

    if not os.path.isfile(a.iso):
        sys.exit(f"ISO not found: {a.iso}")
    if not os.path.isfile(KERNEL_ELF):
        sys.exit(f"kernel ELF not found: {KERNEL_ELF}")

    proc = launch(a.iso, a.gdb_port, mon, serial, vars_fd)
    uaos = Uaos(Monitor(mon), Gdb(a.gdb_port))
    try:
        # --- boot -----------------------------------------------------
        if not wait_serial(serial, "telnetd: listening", 120):
            raise Fail("serial never reached telnetd")

        st = None
        t0 = time.time()
        while time.time() - t0 < 60:
            try:
                st = uaos.gdb.probe_state()
            except Fail:
                pass
            if st and any("Shell" in w["title"] for w in st["wins"]):
                break
            time.sleep(1.5)
        if not (st and any("Shell" in w["title"] for w in st["wins"])):
            raise Fail("Shell window never registered")

        # --- run the demo ----------------------------------------------
        uaos.mon.type_string("SYS:Demos/DevIOTest")
        uaos.mon.send("sendkey ret")

        deadline = time.time() + 120
        verdict = None
        echo = ""
        while time.time() < deadline:
            try:
                log = open(serial, errors="replace").read()
            except FileNotFoundError:
                log = ""
            # RawPutChar emits one char per call and the exec LVO trace
            # interleaves, so the verdict never appears contiguously in the
            # raw log.  Reconstruct the char stream from the trace's
            # `lib=1 fn=98` (exec RawPutChar) request lines — d0 is the
            # emitted byte.
            echo = "".join(
                chr(int(m.group(1), 16))
                for m in re.finditer(
                    r"lib=1 fn=98 d0=0x([0-9A-Fa-f]{8})", log))
            if "DEVIOTEST PASS" in echo or "DEVIOTEST PASS" in log:
                verdict = "PASS"
                break
            if "DEVIOTEST FAIL" in echo or "DEVIOTEST FAIL" in log:
                verdict = "FAIL"
                break
            time.sleep(0.5)

        if verdict == "PASS":
            print("[PASS] DevIOTest reported PASS on serial")
            for line in echo.splitlines():
                if "DEVIOTEST" in line:
                    print(f"  serial: {line.strip()}")
            return 0
        if verdict == "FAIL":
            for line in echo.splitlines():
                if "DEVIOTEST" in line:
                    print(f"  serial: {line.strip()}")
            raise Fail("DevIOTest reported FAIL")
        raise Fail("DEVIOTEST verdict never appeared on serial (timeout)")
    finally:
        if not a.keep:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except Exception:
                proc.kill()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fail as e:
        print(f"[FAIL] {e}")
        sys.exit(1)
