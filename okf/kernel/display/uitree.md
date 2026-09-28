---
type: Kernel Subsystem
title: Declarative UI tree + layout engine (uitree)
description: Dependency-free UINode tree and measure/arrange layout engine shared by kernel tools, libuaos userspace, and host-side tests (UAOS-130, parent UAOS-123).
resource: /kernel/display/uitree.c
tags: [gui, layout, uitree, declarative, gadgets]
status: stable
generated: { by: agent:devin, at: 2026-09-28T23:51:28Z }
---

# Declarative UI tree + layout engine (uitree)

`kernel/display/uitree.{c,h}` implements the shared UI description format for
UAOS: a window's interface is a tree of `UINode`s — container groups
(`UI_VGROUP`, `UI_HGROUP`, `UI_PAGE`) arrange children along a main axis, leaf
nodes are gadgets (`UI_BUTTON`, `UI_CHECKBOX`, `UI_RADIO`, `UI_CYCLE`,
`UI_SLIDER`, `UI_STRING`, `UI_INTEGER`, `UI_LISTVIEW`, `UI_LABEL`,
`UI_SPACER`) or `UI_CUSTOM` app-drawn regions. Modelled on the AmigaOS
ReAction `layout.gadget` / MUI group model.

## Layout model

- `ui_measure()` — bottom-up: each node gets `nat_w`/`nat_h` (natural size
  including `pad`). Leaf text is measured on the 8x16 font grid; groups are
  sum-along-axis / max-across-axis plus `spacing` gaps; `UI_PAGE` adds the
  `UI_PAGE_TAB_H` tab strip and is at least as wide as the tab labels.
- `ui_arrange()` — top-down: each node gets a cell inside its parent group,
  then computes its box (`x/y/w/h`) as the cell inset by `pad`, clamped by
  `max_w/max_h`, and optionally centred at natural size (`UI_F_CENTER`).
- `ui_layout()` — measure + arrange; re-run on every window resize for
  full auto-layout.

Free space on a group's main axis is distributed to children pro-rata by
`weight`; weight-0 children keep their natural size. If no child has
weight > 0, free space stays at the tail (start-packed — use `ui_spacer()`
to push content right/down). When a window is smaller than the measured
tree, children shrink proportionally and still tile flush. The last child
always mops up division rounding so cells tile the group exactly.

## Construction

Nodes come from a caller-supplied `UIArena` (bump allocator over a static
or heap buffer — the kernel has no malloc in display code) via the
`ui_*()` constructors, or can be statically allocated and linked with
`ui_append()`. Varargs container constructors take a NULL-terminated
child list:

```c
UINode *ui = ui_window(&arena, "Pointer",
    ui_vgroup(&arena,
        ui_hgroup(&arena, ui_radio(&a,"16x16",1,1), ...),
        ui_checkbox(&arena, "Shadow", 2),
        ui_spacer(&arena),
        ui_hgroup(&arena, ui_button(&arena,"Save",3), ...),
        NULL));
```

Inline annotation helpers (`ui_w`, `ui_min`, `ui_max`, `ui_pad`,
`ui_spacing`, `ui_flags`, `ui_tab`) wrap constructor calls.
`ui_tab(child, "label")` marks a `UI_PAGE` child's tab title.

## Operations

- `ui_find(root, id)` — depth-first lookup by node id.
- `ui_node_at(root, mx, my)` — deepest node whose box contains the point;
  a `UI_PAGE` tab-strip hit returns the page node, only the active page's
  children are descended into.
- `ui_page_tab_rect(page, i, ...)` — rect of the i-th tab, for backend
  tab rendering/hit-testing.
- `ui_child_count(n)`, `ui_arena_init/alloc/reset`.

## Purity and testing

The file includes only `uitree.h`, `stdarg.h`, `stdint.h`, `stddef.h` —
no `FB_*`, `WM_*`, or libc calls — so it compiles unchanged into the
kernel (`scripts/build_iso.sh` kernel source list), into libuaos
userspace code, and into host tools. `tools/ui_layout_test.c` builds it
with plain gcc and runs 103 assertions (tiling, weight distribution,
shrink, spacers, min/max, centering, pages, hit-tests, arena exhaustion);
the test runs in `build_iso.sh` step 1a and gates the ISO build.

## Related

- [Window Manager](/kernel/display/index.md) — the WM surface a future
  backend binds arranged trees to.
- [intuition.library](/kernel/exec/intuition_lib.md) — BOOPSI dispatch; a
  later `layout.gadget` class will map onto this engine (UAOS-123).
- [ui_layout_test.c](/tools/ui_layout_test.md) — host test harness.
