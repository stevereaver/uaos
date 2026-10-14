#!/usr/bin/env python3
"""qemu_devload_test.py — UAOS-306 regression: real .device loading.

Boots the UAOS ISO headless and runs SYS:Demos/DevLoadTest, a guest-level
M68k program that exercises the real hunk-format .device loader:

  * OpenDevice("uaostest.device") finds no fixed/ROM/generated base and
    falls through to the disk path: LoadSeg("DEVS:uaostest.device"),
    RomTag scan, InitResident (RTF_AUTOINIT -> MakeLibrary + init routine
    + AddDevice), then the device's real Open vector.
  * The guest Open vector vets the unit (5 fails with IOERR_OPENFAIL),
    DeviceList FindName resolves the AddDevice'd node, DoIO/SendIO/
    CheckIO/WaitIO/AbortIO drive the real BeginIO/AbortIO vectors — the
    unit-1 deferred request completes from the device's VERTB interrupt
    server — and a direct jsr -30(a6)/-36(a6) reaches the real Exec
    BeginIO/AbortIO LVOs on both the disk device and timer.device.

DevLoadTest reports via exec/RawPutChar, which shows on serial as one
`lib=1 fn=98 d0=0x<ascii>` trace call per character — the harness
reassembles the stream and requires `DEVLOAD PASS`.  The serial log must
also contain the loader's `[dev] loaded DEVS:uaostest.device` line.

Requires: QEMU, a built ISO + build/uaos_disk.qcow2.
Run:  tests/qemu_devload_test.py [--iso PATH] [--keep] [--timeout SEC]

Exit 0 = all checks passed.
"""

import argparse
import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_layout_test import (Fail, Gdb, Monitor, Uaos, wait_serial,
                              launch, REPO)

ISO_DEFAULT = os.path.join(REPO, "build", "Ultimate_Amiga_OS.iso")
GDB_PORT = 11251

# RawPutChar (-516) is exec fn index 98 in the strace name table; each
# character the guest prints appears as `lib=1 fn=98 d0=0x<ascii>`.
RAWPUTCHAR_RE = re.compile(r"lib=1 fn=98 d0=0x([0-9A-Fa-f]+)")


def decode_rawputchar(log):
    return "".join(chr(int(h, 16)) for h in RAWPUTCHAR_RE.findall(log))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--iso", default=ISO_DEFAULT)
    ap.add_argument("--keep", action="store_true",
                    help="leave QEMU running on exit")
    ap.add_argument("--timeout", type=int, default=180,
                    help="overall seconds to wait for the verdict")
    args = ap.parse_args()

    workdir = tempfile.mkdtemp(prefix="uaos_devload_")
    mon = os.path.join(workdir, "mon")
    serial = os.path.join(workdir, "serial.log")
    vars_fd = os.path.join(workdir, "ovmf_vars.fd")

    proc = launch(args.iso, GDB_PORT, mon, serial, vars_fd)
    uaos = Uaos(Monitor(mon), Gdb(GDB_PORT))
    try:
        if not wait_serial(serial, "telnetd: listening", 120):
            raise Fail("boot timeout waiting for telnetd")

        # Wait for the Shell window before typing the command.
        t0 = time.time()
        while time.time() - t0 < 40:
            try:
                st = uaos.gdb.probe_state()
                if st and any("Shell" in w["title"] for w in st["wins"]):
                    break
            except Fail:
                pass
            time.sleep(1.5)
        else:
            raise Fail("Shell window never registered")

        uaos.mon.type_string("SYS:Demos/DevLoadTest")
        uaos.mon.send("sendkey ret")

        # Poll the decoded RawPutChar stream for the verdict.
        verdict = ""
        log = ""
        t0 = time.time()
        while time.time() - t0 < args.timeout:
            try:
                log = open(serial, errors="replace").read()
            except FileNotFoundError:
                time.sleep(1)
                continue
            verdict = decode_rawputchar(log)
            if "DEVLOAD PASS" in verdict or "DEVLOAD FAIL" in verdict:
                break
            time.sleep(1)

        print("RawPutChar stream:")
        print(verdict)
        print(f"serial log: {serial}")

        if "DEVLOAD FAIL" in verdict:
            raise Fail("guest reported DEVLOAD FAIL")
        if "DEVLOAD PASS" not in verdict:
            raise Fail("no DEVLOAD verdict on serial within timeout")

        # The loader must have taken the real disk path.
        if "[dev] loaded DEVS:uaostest.device" not in log:
            raise Fail("no '[dev] loaded DEVS:uaostest.device' in serial log")

        print("PASS")
    finally:
        if args.keep:
            print(f"--keep: qemu left running, serial={serial}")
        else:
            proc.terminate()


if __name__ == "__main__":
    try:
        main()
    except Fail as e:
        print(f"FAIL: {e}")
        sys.exit(1)
