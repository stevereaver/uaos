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
- [qemu_layout_test.py](/tools/qemu_layout_test.md) (`scripts/`) —
  end-to-end QEMU regression for the M68k BOOPSI layout path: drives
  LayoutTest with closed-loop pointer control via the monitor + GDB stub.

`gen_uaos_native`, `gen_uaos_m68k`, `gen_uaos_x64`, and `gen_m68k_library`
wrap binaries with UAOS headers and generate loadable `.library` stubs —
see [Build System](/build_system.md). `symbolize.sh` maps serial-log crash
addresses back to symbols (see [Kernel](/kernel/index.md) diagnostics).
