#!/usr/bin/env python3
"""analyze_log.py — UAOS serial-log analyzer (UAOS-216)

Paste-in-a-log → annotated report.  Combines:

  * panic/exception dump extraction (the "### EXCEPTION ###" block and
    its surrounding klog tail)
  * RIP/RSP/return-address symbolization via the kernel ELF (same
    nearest-symbol approach as tools/symbolize.sh)
  * warn/err klog rollup — counts per subsystem, first occurrence each
  * stall signatures: watchdog trips, storm-mask events, repeated-boot
    markers

Usage:
    tools/analyze_log.py /tmp/uaos_serial.log [--elf build/uaos-kernel.elf]
    cat log | tools/analyze_log.py
"""

import argparse
import re
import subprocess
import sys
from collections import Counter

HEXRE = re.compile(r"0x[0-9a-fA-F]{6,}")
KVRE = re.compile(r"^\[([a-z0-9_]+)\] (warn|err|crit)\b[: ]*(.*)")


def load_syms(elf):
    syms = []
    try:
        out = subprocess.check_output(
            ["nm", "-n", "--defined-only", elf], text=True)
        for line in out.splitlines():
            p = line.split()
            if len(p) >= 3:
                try:
                    syms.append((int(p[0], 16), p[2]))
                except ValueError:
                    pass
    except Exception:
        pass
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


def annotate(syms, line):
    def rep(m):
        return sym(syms, int(m.group(0), 16))
    return HEXRE.sub(rep, line)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", nargs="?")
    ap.add_argument("--elf", default="build/uaos-kernel.elf")
    ap.add_argument("--tail", type=int, default=25,
                    help="klog lines to show before each fault")
    args = ap.parse_args()

    src = open(args.file) if args.file else sys.stdin
    lines = src.read().splitlines()

    syms = load_syms(args.elf)

    warn_err = Counter()
    first_seen = {}
    faults = []        # (line_idx, kind)
    watchdog = []
    boots = 0

    for i, line in enumerate(lines):
        low = line.lower()
        m = KVRE.match(line)
        if m:
            key = f"{m.group(1)}/{m.group(2)}"
            warn_err[key] += 1
            first_seen.setdefault(key, (i, line.strip()))
        if "exception" in low or "panic" in low or "page fault" in low \
                or "general protection" in low:
            faults.append((i, line.strip()))
        if "watchdog" in low:
            watchdog.append((i, line.strip()))
        if "uaos kernel" in low and "boot" in low:
            boots += 1

    print("=" * 60)
    print(f"analyze_log: {len(lines)} lines, {boots} boot(s)")
    print("=" * 60)

    if warn_err:
        print("\n== warn/err rollup ==")
        for key, n in warn_err.most_common():
            i, text = first_seen[key]
            print(f"  {n:5d}× {key:24s} first@{i}: {text[:80]}")
    else:
        print("\n(no warn/err klog entries)")

    if watchdog:
        print("\n== watchdog events ==")
        for i, text in watchdog[:10]:
            print(f"  @{i}: {text[:100]}")

    if faults:
        print("\n== fault dumps ==")
        for fi, (i, text) in enumerate(faults):
            print(f"\n--- fault {fi} @line {i}: {text}")
            lo = max(0, i - args.tail)
            for j in range(lo, min(len(lines), i + 30)):
                marked = ">" if j == i else " "
                print(f" {marked} {j:5d}| {annotate(syms, lines[j])[:110]}")
    else:
        print("\n(no fault dumps found)")

    # symbolize any bare hex left in the last chunk of the log
    if syms:
        print("\n== symbolized addresses (unique, in-range) ==")
        seen = set()
        for line in lines:
            for m in HEXRE.finditer(line):
                a = int(m.group(0), 16)
                s = sym(syms, a)
                if "+" in s or s in ("", m.group(0)):
                    continue
                if s not in seen and a >= 0x100000:
                    seen.add(s)
                    print(f"  {m.group(0)}  {s}")
                    if len(seen) > 40:
                        break


if __name__ == "__main__":
    main()
