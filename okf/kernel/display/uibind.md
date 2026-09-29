---
type: Kernel Component
title: UI Bind (kernel backend)
description: Binds a UINode declarative tree to a WM window — arranges, renders via the shared gadget set, and routes input as node events.
resource: /kernel/display/uibind.c
tags: [display, gui, wm, gadgets, uitree]
timestamp: 2026-09-29T00:00:00Z
---

# UI Bind (`uibind.c` / `uibind.h`)

UAOS-125. The kernel-side backend of the declarative GUI system
(umbrella UAOS-123): `uibind_open()` takes a `UINode` tree and an event
callback and registers a `WmWindow` that renders and drives it — the tool
declares its interface as a tree and never touches coordinates.

## Model

- **Layout**: `ui_measure()` once at open; `ui_arrange()` re-runs into the
  window client rect on every draw and every mouse event, so resize/zoom
  (`WM_EVT_RESIZE`) reflows gadgets for free — no handler needed.
  `UIBIND_MARGIN` (10 px) insets the tree inside the frame.
- **State**: a `Gad` per interactive node lives inside the `UIBind`
  (parallel `gnodes[]`/`gads[]` arrays, max `UIBIND_MAX_GADS`=32). The
  tree stays a pure description; `Gad` owns `val`/`buf`/`focused`/`sel`.
  String buffers are `UIBIND_STRBUF` (64) char slots per string node.
- **Rendering**: `gad_draw()` per visible gadget; `UI_LABEL` renders as
  text; `UI_CUSTOM` nodes invoke their `UICustomDrawFn`; `UI_PAGE` draws
  a bevelled tab strip (`ui_page_tab_rect` geometry) and only the active
  child's subtree (`node_visible` walks the parent chain).
- **Input**: `ui_node_at()` hit-tests; `gad_event()` mutates state.
  String/integer fields always receive `GAD_DOWN` for focus/defocus;
  sliders are captured in `b->drag` so `GAD_MOVE` tracks outside the
  gadget rect (`gad_slider_from_mouse` direct, bypassing the inside
  check). Radio clicks clear same-group siblings automatically.
- **Pool**: `static UIBind g_binds[UIBIND_MAX_WINDOWS]` (4) — no
  allocation; `uibind_open`/`uibind_close` allocate and free slots.

## App interface

```c
int cb(UIBind *b, UINode *n, int ev, int arg);
```

`ev` is `UIEV_CLICK` (button, on mouse-down), `UIEV_CHANGE` (gadget
state mutated), `UIEV_KEY` (unconsumed keystroke, `arg` = char — e.g.
ESC), `UIEV_RESIZE`, or `UIEV_CLOSE` (close-gadget request; return
non-zero to allow — the bind is then freed and the app must drop its
pointer, zero vetoes).

Helpers: `uibind_gad(b,id)` (raw `Gad*`, e.g. to set `pressed` for
radio-style button groups), `uibind_val`, `uibind_text`,
`uibind_set_val` (repaints, clears radio siblings),
`uibind_repaint` (damage-scoped via `WM_InvalidateRect`),
`uibind_is_open`, `uibind_handle`. `w<=0`/`h<=0` to `uibind_open`
auto-sizes the window to `root->nat_*` + margins; `x<0`/`y<0` centres.

`ui_prefs_row(arena, save_id, use_id, cancel_id)` builds the standard
centred Save/Use/Cancel `UI_HGROUP` (two `ui_spacer`s, 80 px-min buttons).

## Proof-of-concept consumer

`pointer_prefs.c` (UAOS-125) — the whole window is one `ui_window`
tree: three labelled `UI_HGROUP` button rows (weight 1, equal stretch),
a spacer, and a centred Apply/Close row. Button groups use Gad `pressed`
as the selected marker (`sel_group`). ESC maps through `UIEV_KEY`;
the close gadget maps through `UIEV_CLOSE`.
