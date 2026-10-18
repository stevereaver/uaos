#!/usr/bin/env python3
"""qemu_audirq_test.py — UAOS-244 regression: Paula AUDx block-complete IRQs.

Boots the UAOS ISO headless, types `SYS:Demos/AudIrqTest` into the Shell,
and watches the serial log for the demo's verdict.  The demo exercises:

  * Direct AUDxL/AUDxPER/AUDxVOL/AUDxLEN register writes on all 4 channels.
  * Audio DMA sample fetches from the task's guest chip RAM.
  * DMACON master + per-channel enable gating.
  * AUD0-3 block-complete INTREQ bits 7-10 -> level-4 interrupt delivery.
  * ISR re-arm loop sustaining playback for ~2s.

Requires: gdb, QEMU, a built ISO + build/uaos_disk.qcow2.
Run:  tests/qemu_audirq_test.py [--iso PATH] [--timeout SEC]

Exit 0 = AUDIRQ PASS seen on the serial log.
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
    ap.add_argument("--gdb-port", type=int, default=11241)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--keep", action="store_true", help="leave QEMU running")
    a = ap.parse_args()

    mon = "/tmp/uaos_audirq_mon"
    serial = "/tmp/uaos_audirq_serial.log"
    vars_fd = "/tmp/uaos_audirq_ovmf.fd"

    if not os.path.isfile(a.iso):
        sys.exit(f"ISO not found: {a.iso}")
    if not os.path.isfile(KERNEL_ELF):
        sys.exit(f"kernel ELF not found: {KERNEL_ELF}")

    proc = launch(a.iso, a.gdb_port, mon, serial, vars_fd)
    uaos = Uaos(Monitor(mon), Gdb(a.gdb_port))
    try:
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

        uaos.mon.type_string("SYS:Demos/AudIrqTest")
        uaos.mon.send("sendkey ret")

        deadline = time.time() + 120
        verdict = None
        echo = ""
        while time.time() < deadline:
            try:
                log = open(serial, errors="replace").read()
            except FileNotFoundError:
                log = ""
            echo = "".join(
                chr(int(m.group(1), 16))
                for m in re.finditer(
                    r"lib=1 fn=98 d0=0x([0-9A-Fa-f]{8})", log))
            if "AUDIRQ PASS" in echo or "AUDIRQ PASS" in log:
                verdict = "PASS"
                break
            if "AUDIRQ FAIL" in echo or "AUDIRQ FAIL" in log:
                verdict = "FAIL"
                break
            time.sleep(0.5)

        if verdict == "PASS":
            print("[PASS] AudIrqTest reported PASS on serial")
            for line in echo.splitlines():
                if "AUDIRQ" in line or "aud" in line:
                    print(f"  serial: {line.strip()}")
            return 0
        if verdict == "FAIL":
            for line in echo.splitlines():
                if "AUDIRQ" in line or "aud" in line:
                    print(f"  serial: {line.strip()}")
            raise Fail("AudIrqTest reported FAIL")
        raise Fail("AUDIRQ verdict never appeared on serial (timeout)")
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
