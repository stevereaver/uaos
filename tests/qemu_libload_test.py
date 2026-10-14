#!/usr/bin/env python3
"""qemu_libload_test.py — UAOS-305 regression: real .library loading.

Boots the UAOS ISO headless and runs SYS:Demos/LibLoadTest, a guest-level
M68k program that exercises the real hunk-format .library loader:

  * OpenLibrary("uaostest.library") finds no fixed/ROM/generated base and
    falls through to the disk path: LoadSeg("LIBS:uaostest.library"),
    RomTag scan, InitResident (RTF_AUTOINIT -> MakeLibrary + init routine
    + AddLibrary), then the library's real Open vector.
  * LibList FindName, version/revision/OpenCnt fields, all lib vectors
    (Add/GetMagic/ExecProbe/GetData/Counter), reopen/close semantics,
    version gating and "LIBS:" path normalisation.

LibLoadTest reports via exec/RawPutChar, which shows on serial as one
`lib=1 fn=98 d0=0x<ascii>` trace call per character — the harness
reassembles the stream and requires `LIBLOAD PASS`.  The serial log must
also contain the loader's `[lib] loaded LIBS:uaostest.library` line.

Requires: QEMU, a built ISO + build/uaos_disk.qcow2.
Run:  tests/qemu_libload_test.py [--iso PATH] [--keep] [--timeout SEC]

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
GDB_PORT = 11250

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

    workdir = tempfile.mkdtemp(prefix="uaos_libload_")
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

        uaos.mon.type_string("SYS:Demos/LibLoadTest")
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
            if "LIBLOAD PASS" in verdict or "LIBLOAD FAIL" in verdict:
                break
            time.sleep(1)

        print("RawPutChar stream:")
        print(verdict)
        print(f"serial log: {serial}")

        if "LIBLOAD FAIL" in verdict:
            raise Fail("guest reported LIBLOAD FAIL")
        if "LIBLOAD PASS" not in verdict:
            raise Fail("no LIBLOAD verdict on serial within timeout")

        # The loader must have taken the real disk path.
        if "[lib] loaded LIBS:uaostest.library" not in log:
            raise Fail("no '[lib] loaded LIBS:uaostest.library' in serial log")

        # Any real third-party libraries dropped into LIBS: show up as
        # LIBLOAD INFO lines; when they are absent the fake bases are
        # skipped silently — either way this must not break the test.
        for m in re.finditer(r"LIBLOAD INFO ([^\n]+)", verdict):
            print(f"  info: {m.group(1)}")

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
