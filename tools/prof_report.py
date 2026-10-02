#!/usr/bin/env python3
"""prof_report.py — symbolize a UAOS prof dump (UAOS-210)

`prof FILE=path` in-guest writes text lines "rip_hex taskidx count"
(kernel/dbg/prof.c, Prof_DumpFile).  This script aggregates by symbol
and prints a sorted hotspot table:

    tools/prof_report.py prof.txt [--elf build/uaos-kernel.elf] [--top 20]
"""

import argparse
import subprocess
import sys


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
        print(f"prof_report: cannot read {elf}: {e}", file=sys.stderr)
    return syms


def sym(syms, addr):
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
    ap.add_argument("--top", type=int, default=20)
    args = ap.parse_args()

    syms = load_syms(args.elf)
    buckets = {}
    total = 0
    for line in open(args.file):
        parts = line.split()
        if len(parts) < 3:
            continue
        try:
            rip = int(parts[0], 16)
            count = int(parts[2])
        except ValueError:
            continue
        name = sym(syms, rip)
        buckets[name] = buckets.get(name, 0) + count
        total += count

    if not total:
        sys.exit("prof_report: no samples in file")

    print(f"{total} samples")
    for name, count in sorted(buckets.items(),
                              key=lambda kv: -kv[1])[: args.top]:
        print(f"  {count * 100.0 / total:6.2f}%  {count:8d}  {name}")


if __name__ == "__main__":
    main()
