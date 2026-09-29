---
type: Concept
title: .gui Interface File Format
description: Text format that parses into the same UINode tree the C API builds, loaded from SYS:Prefs/GUI/ with an ENV:GUI/ runtime override.
tags: [gui, uitree, format, parser, uibind, prefs]
timestamp: 2026-09-29T00:00:00Z
---

# .gui Interface File Format (UAOS-127)

`kernel/display/uiformat.{c,h}` implements `ui_parse()`: a dependency-free
recursive-descent parser that turns a text description into the identical
`UINode` tree the `ui_*()` constructors build. Tools can therefore ship
their interface as data and iterate on layout without recompiling; it is
also the output format for the in-OS GUI designer (UAOS-129).

The full grammar lives in the `uiformat.h` header comment. Summary:

```
window "Title" {
  vgroup spacing=6 {
    label "Cursor size" ;
    hgroup weight=1 {
      button "16x16" id=size16 selected ;
      button "32x32" id=size32 ;
    }
    slider id=accel 0..100 value=50 ;
    string "demo" maxchars=24 id=name ;
    page { tab "One" { ... } tab "Two" { ... } }
  }
}
```

- Leaves end in `;`, containers use `{ }`, `//` is a line comment.
- Attributes: `id=`, `weight=`/`w=`, `min=W,H`, `max=W,H`, `pad=`,
  `spacing=`, `group=`, `tab=`, `active=`, `sel=`, `value=`, `maxchars=`,
  `text=`, plus flag words `disabled readonly toggle selected center
  vertical`.
- `id=name` hashes via FNV-1a (`ui_sym()` in uitree.h) so C code resolves
  the same integer with `ui_sym("name")`; `id=7` stays numeric.
- Strings accept `"..."` or `'...'` (single quotes survive `echo`/`ed`
  entry since the shell strips double quotes).
- `page` children are `tab "Label" { ... }` blocks (implicit vgroup).
- `custom` nodes carry only rect hints — apps attach draw/hit callbacks
  after parsing via `ui_find()`.

## Parser model

- `ui_parse(UIArena *a, char *text, UIParseErr *err)` — mutates `text`
  (strings are NUL-terminated in place, escapes compacted); nodes and
  item arrays are bump-allocated from `a`. No libc, no heap: compiles
  into the kernel, libuaos and the host test.
- Errors return NULL with `UIParseErr{line,col,msg}` filled.
- Positional payload and attributes may be mixed in any order
  (`slider id=1 0..100 value=50` ≡ `slider 0..100 id=1 value=50`).

## Loading convention

Tools look up their description by name; a missing or unparsable file
never bricks a tool — they fall back to the compiled-in tree.

- Kernel (`uibind.h`): `uibind_load_gui(&arena, "pointer", fallback)`
  tries `ENV:GUI/<tool>.gui` then `SYS:Prefs/GUI/<tool>.gui`. The file
  text is read into an arena-allocated buffer (in-place strings share
  the tree's lifetime); the arena is rewound on parse failure.
- Userspace (`uaos_ui.h`): `uaos_ui_load(&arena, "tool", fallback)`
  does the same over `uaos_stat`/`uaos_open`/`uaos_read_file`.
- `ENV:` maps to `RAM:ENV`, so `ENV:GUI/x.gui` is a scratch override
  that vanishes on reboot; `SYS:Prefs/GUI/` is the shipped location
  (`system/Prefs/GUI/` in the repo, staged onto the ISO).

Shipped descriptions: `SYS:Prefs/GUI/pointer.gui` (kernel) and
`SYS:Prefs/GUI/uidemo.gui` (userspace). Node ids are numeric and must
match the consumer's enum — see the comment header in each file.

## Testing

`tools/ui_layout_test.c` round-trips a .gui document against the
equivalent C-built tree — `cmp_tree` asserts every node's type/id/
weight/rect/natural size matches at two window sizes — plus all-kind
payload checks and error-position checks (433 checks total, gated in
`scripts/build_iso.sh` step 1a).
