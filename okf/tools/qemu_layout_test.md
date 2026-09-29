---
type: Tool
title: qemu_layout_test.py
description: End-to-end QEMU regression test for the M68k BOOPSI layout path — drives LayoutTest via the QEMU monitor + GDB stub with closed-loop pointer control.
resource: /scripts/qemu_layout_test.py
tags: [tool, test, qemu, m68k, boopsi, layout]
status: stable
generated: { by: agent:devin, at: 2026-09-29T08:00:00Z }
---

# qemu_layout_test.py

Scripted regression harness for the M68k GUI/BOOPSI path (covers UAOS-128).
Stdlib-only Python 3; owns its own QEMU instance (GDB stub :11234, monitor
unix socket, serial log — all under /tmp, isolated from manual sessions).

## Mechanism

- **Input**: human-monitor `sendkey` (types `SYS:Demos/LayoutTest` into the
  console shell) and `mouse_move`/`mouse_button` for the PS/2 pointer.
- **Closed-loop positioning**: PS/2 deltas are relative, so after every move
  the harness reads the kernel's `g_mouse.x/y` through the GDB stub and
  iterates until within tolerance. No pixel matching anywhere.
- **Assertions**: symbolic reads via `gdb -batch` on
  `build/uaos-kernel.elf` — `g_wins[]` (WM titles/rects), `g_intu_wins[]`
  (guest window pointers), `g_tasks[]` (m68k task `m68k_ram` base), then a
  big-endian walk of the guest gadget list through that RAM buffer.
- **GDB scripts** go through `-x` files (per-line `-ex` cannot express
  `while`/`if` blocks) and decode guest memory by byte-array indexing —
  `define`d helper args are mis-parsed by gdb when unparenthesized.

## Checks (12)

boot serial → telnetd; Shell window registered; LayoutTest window + m68k
task spawn; all demo children spliced (IDs 101–106) with nested hgroup
row geometry; East/West/checkbox clicks produce the right `SetWindowTitles`
feedback; `GFLG_SELECTED` persists on the toggle; sizing-gadget drag grows
the window and reflows the layout; Quit exits cleanly; serial stays free
of cycle-budget aborts, wild PCs, and faults.

## Usage

```bash
scripts/qemu_layout_test.py            # builds nothing; uses existing ISO
scripts/qemu_layout_test.py --keep     # leave QEMU running for inspection
```

Exit 0 = all pass. On failure a screendump lands at
`/tmp/uaos_layout_test.ppm`. First-run result: caught a real bug — the
`WM_EVT_MOUSE_UP` sweep cleared `GFLG_SELECTED` on the checkbox it had
just toggled (fixed in `intuition_lib.c`).
