#!/usr/bin/env python3
"""qemu_optlibs_test.py — UAOS-249 regression: optional OctaMED libraries.

Boots the UAOS ISO headless and runs SYS:Demos/OptLibsTest, a guest-level
M68k program that verifies the optional AmigaOS modules OctaMED probes
for either work or fail cleanly:

  * OpenLibrary("amigaguide.library")   -> NULL
  * OpenLibrary("libs:amigaguide...")   -> NULL (basename match)
  * OpenLibrary("powerpacker.library")  -> NULL
  * OpenLibrary("lh.library")           -> NULL
  * OpenLibrary("rexxsyslib.library")   -> NULL
  * OpenLibrary("diskfont.library")     -> ROM stub opens; OpenDiskFont
    -> NULL and AvailFonts -> afh_NumEntries=0
  * OpenDevice("serial.device")         -> io_Error = IOERR_OPENFAIL
  * OpenLibrary("utility.library")      -> still opens (unaffected)

OptLibsTest reports via exec/RawPutChar, which shows on serial as one
`lib=1 fn=98 d0=0x<ascii>` trace call per character — the harness
reassembles the stream and requires `OPTLIBS PASS`.

Requires: QEMU, a built ISO + build/uaos_disk.qcow2.
Run:  tests/qemu_optlibs_test.py [--iso PATH] [--keep] [--timeout SEC]

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
GDB_PORT = 11244

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

    workdir = tempfile.mkdtemp(prefix="uaos_optlibs_")
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

        uaos.mon.type_string("SYS:Demos/OptLibsTest")
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
            if "OPTLIBS PASS" in verdict or "OPTLIBS FAIL" in verdict:
                break
            time.sleep(1)

        print("RawPutChar stream:")
        print(verdict)

        if "OPTLIBS FAIL" in verdict:
            raise Fail("guest reported OPTLIBS FAIL")
        if "OPTLIBS PASS" not in verdict:
            raise Fail("no OPTLIBS verdict on serial within timeout")

        # Declined opens must show as MISSING in the OpenLibrary trace.
        for name in ("amigaguide", "powerpacker", "lh", "rexxsyslib"):
            if not re.search(
                    rf'OpenLibrary\("[^"]*{name}[^"]*",[^)]*\)-> MISSING', log):
                raise Fail(f"no MISSING trace for {name} in serial log")

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
