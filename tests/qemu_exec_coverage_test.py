#!/usr/bin/env python3
"""qemu_exec_coverage_test.py — UAOS-239 regression: exec.library coverage.

Boots the UAOS ISO headless and runs SYS:Demos/ExecTest, a guest-level
M68k program that directly exercises the exec vectors UAOS-239 added
(Supervisor, SetSR/GetCC, AllocMem/AllocAbs, Allocate/Deallocate,
MakeFunctions/MakeLibrary, FindResident/InitResident, Alert/Debug,
AddTask/RemTask/FindTask, AddLibrary/RemLibrary, AddDevice/RemDevice,
AddResource/RemResource, SumLibrary, semaphore lists, pools,
RawIOInit/RawPutChar, mem handlers, child stubs, ...).

ExecTest reports via exec/RawPutChar, which shows on serial as one
`lib=1 fn=98 d0=0x<ascii>` trace call per character — the harness
reassembles the stream and requires `EXECTEST PASS`.

Requires: QEMU, a built ISO + build/uaos_disk.qcow2.
Run:  tests/qemu_exec_coverage_test.py [--iso PATH] [--keep] [--timeout SEC]

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
GDB_PORT = 11243

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

    workdir = tempfile.mkdtemp(prefix="uaos_exec_")
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

        uaos.mon.type_string("SYS:Demos/ExecTest")
        uaos.mon.send("sendkey ret")

        # Poll the decoded RawPutChar stream for the verdict.
        verdict = ""
        t0 = time.time()
        while time.time() - t0 < args.timeout:
            try:
                log = open(serial, errors="replace").read()
            except FileNotFoundError:
                time.sleep(1)
                continue
            verdict = decode_rawputchar(log)
            if "EXECTEST PASS" in verdict or "EXECTEST FAIL" in verdict:
                break
            time.sleep(1)

        print("RawPutChar stream:")
        print(verdict)

        if "EXECTEST FAIL" in verdict:
            raise Fail("guest reported EXECTEST FAIL")
        if "EXECTEST PASS" not in verdict:
            raise Fail("no EXECTEST verdict on serial within timeout")

        # The run must not hit any unknown/unimplemented exec vector.
        unimpl = re.findall(r"\[exec\] (unimpl|unknown)[^\n]*", log)
        if unimpl:
            raise Fail(f"{len(unimpl)} unimplemented exec hits: {unimpl[:5]}")

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
