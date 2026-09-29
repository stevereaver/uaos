---
type: Kernel Component
title: Shared Gadget Set
description: Single gadget descriptor + draw/hit-test/event code shared by all kernel-native tools (prefs editors, requesters, format/exchange windows).
resource: /kernel/display/gadgets.c
tags: [display, gui, gadgets, widgets]
timestamp: 2026-09-29T00:00:00Z
---

# Shared Gadget Set (`gadgets.c` / `gadgets.h`)

UAOS-124. One gadget vocabulary for every kernel-native tool, extracted from the
private widget copies that used to live in `prefs_win.c` (`PwBtn`/`PwCycle`/
`PwSlider`/`PwCheck`), `requester.c`, `format_win.c`, `exchange_win.c`, and
`pointer_prefs.c` (each had its own button struct and bevel code).

## Descriptor

```c
typedef struct Gad {
    int kind;              /* GAD_BUTTON, GAD_CHECKBOX, GAD_RADIO, GAD_CYCLE,
                              GAD_SLIDER, GAD_STRING, GAD_INTEGER, GAD_LABEL,
                              GAD_GBOX, GAD_LISTVIEW */
    int x, y, w, h;        /* screen-space rect, set by owner draw fn */
    const char *text;      /* label / button text */
    const char **choices;  /* cycle + listview items */
    int nchoices;
    int val, sel;          /* cycle index / checkbox state / listview sel */
    int min, max;          /* slider range */
    int pressed, focused;  /* visual / input state */
    int group;             /* radio mutual-exclusion group id */
    char *buf;             /* string gadget text buffer */
    int buf_len, buf_max, cursor;
    uint32_t flags;        /* GADF_VERTICAL, GADF_DISABLED */
} Gad;
```

Statically allocated by each tool (no allocation inside `gadgets.c`).

## API

- `gad_draw(g)` — per-kind renderer, WB 3.1 metrics (8×16 text grid, bevel
  chrome, ~22 px buttons, 14 px checkboxes).
- `gad_hit(g, mx, my)` — rect hit test; checkbox/radio include label width.
- `gad_event(g, phase, a, b)` — `GAD_DOWN/GAD_MOVE/GAD_UP` take (mx,my),
  `GAD_KEY` takes the char. Returns `GADE_CLICK`/`GADE_CHANGE`/`GADE_NONE`.
- `gad_slider_from_mouse(g, m)` — position→value for drag updates.
- Chrome helpers: `gad_bevel`, `gad_label`, `gad_gbox`, `gad_bg`,
  `gad_btn_row` (the standard Apply/Save/Close row: centres three 96×22
  gadgets `GAD_BTN_GAP` apart at the window bottom).
- Text helpers: `gad_slen`, `gad_str_cp`, `gad_str_eq`, `gad_itoa`.

## Behaviour notes

- **Buttons** fire on mouse-`GAD_DOWN` inside (matches the old `PwBtn`
  behaviour); `pressed` is caller-managed for tools that want press/release
  visuals or a selected-state highlight (pointer prefs uses `pressed` as
  its radio-style "active" marker).
- **Cycle** — click on the arrow column's lower half steps backward,
  anywhere else steps forward; `g->val` owns the selected index (the old
  per-editor `*_idx` globals are gone).
- **Slider** — `GADF_VERTICAL` selects vertical; knob travel is `len - 16`.
- **String/Integer** — `focused` gates editing; `string_key` handles
  insert/delete/cursor keys; `GAD_INTEGER` filters to digits. Buffer is
  caller-supplied via `buf`/`buf_max`.
- **Radio** — `gad_event` only sets `val=1`; clearing siblings is the
  owner's job (same as the old code).
- Tools that draw into a `WM` client area set gadget rects inside their
  draw function (rects are re-asserted every repaint — safe for window
  moves).

## Migrated call sites

`prefs_win.c` (all editors: Palette, IControl, Input, ScreenMode, WBPattern,
Font, Serial, Printer, Locale, Time), `requester.c` (info/confirm/string
buttons + the editable string field), `format_win.c` (device cycle, volume
name, buttons), `exchange_win.c` (button row), `pointer_prefs.c` (button
groups). No private gadget structs or draw code remain in those files.

## Related incident fixed during verification (not gadget code)

QEMU testing exposed a pre-existing stack bomb: `PrefsFile`
(`kernel/exec/prefs_lib.h`) is ~128 KB (`PREFS_MAX_CHUNKS` × 4 KB data) and
`Prefs_Load` `memset`s the whole struct. Every caller instantiated it as a
stack local on a 32 KB-or-smaller task stack, so each `Prefs_Load` wiped
~96 KB of memory above the caller's stack — landing on `g_task_stacks` after
the BSS layout shifted (linking `gadgets.o`/`uitree.o`), which zeroed a live
interrupt frame on EventPump's stack and caused a `#GP` on `iretq`.
Fixed by moving all four `PrefsFile` instances to `static` storage
(`prefs_win.c` ×2, `prefs_lib.c` ×2) and documenting the constraint in
`prefs_lib.h`. **Never declare `PrefsFile` as a local.**
