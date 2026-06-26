---
type: Kernel Library
title: gadtools.library
description: Native host thunk implementation of AmigaOS gadtools.library gadget creation and layout helpers.
resource: /kernel/exec/gadtools_lib.c
tags: [gadtools, gadgets, intuition, m68k, thunk]
timestamp: 2026-07-10T17:00:00Z
---

# gadtools.library

UAOS provides a native host implementation of `gadtools.library` that builds standard Intuition gadgets from AmigaOS `NewGadget` and tag-list descriptions. The resulting gadgets are stored in guest memory and rendered by the existing Intuition gadget paths, so no separate renderer is required.

## Core responsibilities

- Create high-level gadgets from `NewGadget` templates: `CreateGadgetA()` / `CreateGadget()`.
- Create a gadget context list root: `CreateContext()`.
- Free entire gadget chains: `FreeGadgets()`.
- Get and set gadget attributes at runtime: `GT_SetGadgetAttrsA()` / `GT_GetGadgetAttrsA()`.
- Provide screen-specific drawing information: `GetVisualInfoA()` / `FreeVisualInfo()`.
- Relay Intuition/Exec message helpers to the existing host implementations: `GT_GetIMsg()`, `GT_ReplyIMsg()`, `GT_BeginRefresh()`, `GT_EndRefresh()`.
- Create GadTools menu trees from `NewMenu` arrays: `CreateMenusA()` / `FreeMenus()`, `LayoutMenuItemsA()`, `LayoutMenusA()`.
- Bevel-box function is currently stubbed: `DrawBevelBoxA()`.

## Implemented gadget kinds

| Kind | Underlying Intuition gadget | Notes |
|------|----------------------------|-------|
| `BUTTON_KIND` | `GTYP_BOOLGADGET` | Plain push-button; label rendered from `ng_GadgetText`. |
| `CHECKBOX_KIND` | `GTYP_BOOLGADGET` | Toggle-select boolean gadget. |
| `CYCLE_KIND` | `GTYP_BOOLGADGET` | Stores label array/active index in `UserData`. |
| `MX_KIND` / `RADIO_KIND` | `GTYP_BOOLGADGET` | Mutual-exclude flag set via `MutualExclude`. |
| `SLIDER_KIND` | `GTYP_PROPGADGET` | `GTSL_Min`/`Max`/`Level` mapped to horizontal pot value. |
| `STRING_KIND` | `GTYP_STRGADGET` | `StringInfo` with `GTST_String`/`GTST_MaxChars`. |
| `INTEGER_KIND` | `GTYP_INTGADGET` | Same as string gadget but initialised from `GTIN_Number`. |
| `LISTVIEW_KIND` | `GTYP_LISTVIEW` | UAOS simple listview extension (items/count/selected/top). |
| `NUMBER_KIND` / `TEXT_KIND` | `GTYP_BOOLGADGET` | Non-interactive display gadget; activation cleared. |

## Key structures and constants

All AmigaOS-compatible offsets and tag values are defined in `kernel/exec/gadtools_lib.h`:

- `NewGadget` structure offsets (`NG_*`).
- `NG_*` flag bits (`NG_LOWLABEL`, `NG_TOGGLE`, `NG_DISABLED`, etc.).
- Gadget kind numbers (`BUTTON_KIND`, `CHECKBOX_KIND`, etc.).
- GadTools tag base and per-kind tags (`GT_TagBase`, `GTST_String`, `GTLV_Labels`, `GTSL_Min`, etc.).
- `NewMenu` structure offsets (`NM_OFF_*`) and type constants (`NM_TITLE`, `NM_ITEM`, `NM_SUB`, `NM_END`).
- BOOPSI gadget attributes (`GA_*`), including `GA_Disabled`.
- `VisualInfo` layout (`GTVI_*`).
- Function indices (`GADTOOLS_OPEN_LIBRARY` through `GADTOOLS_GT_GET_GADGET_ATTRS_A`).

## Implementation files

- `kernel/exec/gadtools_lib.h` — public constants and register/dispatch prototype.
- `kernel/exec/gadtools_lib.c` — gadget creation, attribute dispatch, and `VisualInfo` allocation.
- `emulation/uaos_m68k_glue.c` — LVO jump-table wiring and ILLEGAL-opcode dispatcher for `lib==LIB_GADTOOLS`.
- `kernel/exec/rom_modules.c` — registers `gadtools.library` at boot via `UAOS_GADTOOLS_Register()`.
- `scripts/build_iso.sh` — compiles `gadtools_lib.c` and links it into the ELF64 kernel.

## Memory allocation

GadTools reuses the guest heap allocator exposed in `kernel/exec/intuition_lib.h`:

- `uint32_t intu_alloc(uint32_t size)` — allocate guest memory.
- `void intu_free(uint32_t user_addr)` — free guest memory.

These are used for gadget structures, `StringInfo`, `IntuiText` labels, and the `VisualInfo` `DrawInfo` block.

## Reusing shared helpers

`gadtools_lib.c` calls the same shared utilities as `intuition_lib.c`:

- `UAOS_Intuition_Dispatch()` for `GT_BeginRefresh()` and `GT_EndRefresh()`.
- `UAOS_Exec_Dispatch()` for `GT_GetIMsg()` and `GT_ReplyIMsg()` (Exec message port functions).
- Host `g_ram` accessors and guest string helpers replicated locally to avoid pulling in large Intuition private headers.

## GadTools menus

`CreateMenusA(newmenu, tags)` walks a `NewMenu` array (22-byte packed AmigaOS layout) and builds a classic `Menu`/`MenuItem` tree in guest memory:

- `NM_TITLE` entries allocate a `Menu` with an `IntuiText` menu name and `MENUENABLED`.
- `NM_ITEM` entries allocate `MenuItem` nodes linked under the current menu.
- `NM_SUB` entries allocate `MenuItem` nodes linked as the `SubItem` chain of the current item.
- `NM_END` terminates the walk.
- Each item stores the `NewMenu` flags (`ITEMTEXT`, `ITEMENABLED`, `COMMSEQ`, `CHECKIT`, `MENUTOGGLE`), `MutualExclude`, and creates an `IntuiText` label. A command key is written to `MenuItem.Command` when `COMMSEQ` is set.

`FreeMenus(menu)` walks the `Menu`/`MenuItem` tree (including sub-item chains) and frees the `IntuiText` labels plus the structures via the guest heap.

`LayoutMenuItemsA(firstItem, vi, tags)` measures each item's label using the host 8x16 font and sets `MenuItem.LeftEdge/TopEdge/Width/Height` with a vertical item layout. Sub-items are laid out recursively to the right of their parent item.

`LayoutMenusA(firstMenu, vi, tags)` lays out menu titles horizontally across the menu bar and calls `LayoutMenuItemsA` for each menu's first item. Each menu width is expanded to fit the widest item if necessary.

## Current limitations

- `DrawBevelBoxA()` is a stub; bevel boxes are not rendered.
- `GT_FilterIMsg()` / `GT_PostFilterIMsg()` are pass-throughs; no keyboard/mouse filtering is applied.
- `NUMBER_KIND` and `TEXT_KIND` are rendered as non-interactive boolean gadgets (a visual placeholder).
- ListView supports single selection only; `GTLV_ReadOnly` and multi-select are not yet implemented.
- Slider is horizontal only; vertical sliders and custom level formatting are not yet implemented.

## Build verification

After modifying the GadTools code or adding new gadget kinds, rebuild the ISO:

```bash
./scripts/build_iso.sh
```

A successful build should produce `build/Ultimate_Amiga_OS.iso`.
