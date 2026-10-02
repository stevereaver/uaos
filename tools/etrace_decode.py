#!/usr/bin/env python3
"""etrace_decode.py — decode a UAOS kernel event-trace dump (UAOS-209)

The in-guest command `etrace FILE=path` writes the binary ring
(kernel/dbg/etrace.c, Etrace_DumpFile):

    header:  u8[4] 'ETRC' | u16 version | u16 reserved | u32 count  (12 B)
    record:  u64 tsc | u16 event | u16 pad | u32 arg0 | u32 arg1 | pad4
             — sizeof(EtraceRec) = 24 B (u64-aligned struct write)

Usage:
    tools/etrace_decode.py trace.bin [--elf build/uaos-kernel.elf]
                                   [--hz 3.0e9]

Prints one line per record; RIP-shaped arguments are symbolized when an
ELF with symbols is available (nm, same idea as tools/symbolize.sh).
"""

import argparse
import struct
import subprocess
import sys

MAGIC = b"ETRC"
HDR_LEN = 12
REC_LEN = 24   # sizeof(EtraceRec) with 8-byte alignment tail padding

EVENTS = {
    1: ("IRQ_ENTER", "vec", "a1"),
    2: ("IRQ_EXIT",  "vec", "cyc_lo"),
    3: ("SCHED",     "from_idx", "to_idx"),
    4: ("SIGNAL",    "taskidx", "sigmask"),
    5: ("DOPKT",     "type", "port"),
    6: ("PKT_TX",    "len", "proto"),
    7: ("PKT_RX",    "len", "proto"),
}


def load_syms(elf):
    syms = []
    try:
        out = subprocess.check_output(
            ["nm", "-n", "--defined-only", elf], text=True)
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 3:
                try:
                    syms.append((int(parts[0], 16), parts[2]))
                except ValueError:
                    pass
    except Exception as e:
        print(f"etrace_decode: cannot read symbols: {e}", file=sys.stderr)
    return syms


def sym(syms, addr):
    if not (0x100000 <= addr < 0x80000000):
        return f"0x{addr:x}"
    lo, hi, best = 0, len(syms), None
    while lo < hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            best = syms[mid]; lo = mid + 1
        else:
            hi = mid
    if not best:
        return f"0x{addr:x}"
    off = addr - best[0]
    return f"{best[1]}+0x{off:x}" if off else best[1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--elf", default="build/uaos-kernel.elf")
    ap.add_argument("--hz", type=float, default=0,
                    help="TSC Hz for a delta-us column (0 = raw deltas)")
    args = ap.parse_args()

    data = open(args.file, "rb").read()
    if len(data) < HDR_LEN:
        sys.exit("etrace_decode: file too small")
    if data[:4] != MAGIC:
        sys.exit(f"etrace_decode: bad magic {data[:4]!r} (want {MAGIC!r})")
    ver, _rsv, count = struct.unpack_from("<HHI", data, 4)

    syms = load_syms(args.elf) if args.elf else []
    print(f"# ETRC v{ver}  records={count}  (args raw; use --hz for us deltas)")

    prev_tsc = None
    off = HDR_LEN
    for i in range(count):
        if off + REC_LEN > len(data):
            print(f"-- truncated at record {i} --")
            break
        tsc, ev, _pad, a0, a1 = struct.unpack_from("<QHHII", data, off)
        off += REC_LEN
        name, n0, n1 = EVENTS.get(ev, (f"EV{ev}", "a0", "a1"))
        dt = ""
        if prev_tsc is not None:
            d = tsc - prev_tsc
            dt = f"{d / args.hz * 1e6:10.1f}us" if args.hz else f"+{d}"
        prev_tsc = tsc
        s0 = sym(syms, a0) if n0 in ("rip", "a1") and a0 else f"0x{a0:x}"
        s1 = sym(syms, a1) if n1 == "rip" else f"0x{a1:x}"
        print(f"[{i:5d}] tsc={tsc:016x} {dt:>14s} {name:9s} {n0}={s0} {n1}={s1}")


if __name__ == "__main__":
    main()
