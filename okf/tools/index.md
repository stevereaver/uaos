---
type: OKF Index
title: Host Tools
description: Host-side build utilities and test harnesses in tools/.
resource: /okf/tools/
tags: [index, tools, build, test]
generated: { by: agent:devin, at: 2026-09-28T23:51:28Z }
---

# Host Tools

Programs under `tools/` that run on the build host — binary wrappers used by
`scripts/build_iso.sh` plus test harnesses for kernel-pure code.

- [ui_layout_test.c](/tools/ui_layout_test.md) — unit tests for the
  `uitree` layout engine; runs in `build_iso.sh` step 1a and gates the ISO
  build.
- [qemu_layout_test.py](/tools/qemu_layout_test.md) (`tests/`) —
  end-to-end QEMU regression for the M68k BOOPSI layout path: drives
  LayoutTest with closed-loop pointer control via the monitor + GDB stub.
- [make_splash.py](/tools/make_splash.md) — converts `splash.jpg` into a
  self-describing RGB24 blob linked into the kernel (boot splash) via
  `ld -r -b binary`; run by `build_iso.sh`.

`gen_uaos_native`, `gen_uaos_m68k`, `gen_uaos_x64`, and `gen_m68k_library`
wrap binaries with UAOS headers and generate loadable `.library` stubs —
see [Build System](/build_system.md). `symbolize.sh` maps serial-log crash
addresses back to symbols (see [Kernel](/kernel/index.md) diagnostics).

Diagnostics host tooling (UAOS-188, see [Diagnostics toolkit](/kernel/diag/index.md)):

- `tools/gdb_uaos.py` — GDB Python helpers for the QEMU stub session
  (`uaos tasks`, `uaos task NAME`, `uaos timers`, `uaos stack NAME`);
  walks `g_tasks[]` via DWARF and decodes parked interrupt frames.
- `tools/etrace_decode.py` — decodes the `etrace FILE=` binary ring dump
  (`ETRC` magic, 12 B header + 24 B records) with optional symbolization.
- `tools/prof_report.py` — symbolizes `prof FILE=` output
  (`rip taskidx count` rows) into a hotspot table.
- `tools/analyze_log.py` — serial-log analyzer: panic extraction and
  symbolization, warn/err rollup, watchdog events, boot markers.
- `tests/smoke.sh` — headless QEMU + telnet regression battery:
  runs a command list, asserts output, archives serial log + pcap to
  `build/smoke-<ts>/`.
