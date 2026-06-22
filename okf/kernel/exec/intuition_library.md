---
type: Kernel Library
title: intuition.library
description: UAOS native implementation of the AmigaOS intuition.library for emulated M68k tasks.
resource: /kernel/exec/intuition_lib.c
tags: [intuition, library, m68k, thunking, window, wm]
timestamp: 2026-06-22T16:00:00Z
---

# intuition.library

`intuition.library` provides the classic AmigaOS windowing API to emulated M68k tasks. The UAOS implementation is a native thunk layer that translates guest `OpenWindow`/`CloseWindow` and related calls into the host window manager API.

## Key files

- `kernel/exec/intuition_lib.c` — native implementations and function table.
- `kernel/exec/intuition_lib.h` — minimal guest `Window` / `NewWindow` structures and flags.
- `kernel/display/wm.h` — host window manager API.
- `emulation/uaos_m68k_glue.c` — M68k LVO stub installation and dispatch.

## Implementation status

### Window lifecycle

| Function | Status | Notes |
|----------|--------|-------|
| `OpenWindow` | Implemented | Translates a guest `NewWindow` into a host WM window and allocates a guest `Window` + `RastPort`. Width/height are clamped to the `NewWindow` `MinWidth/MinHeight` and `MaxWidth/MaxHeight` bounds. |
| `OpenWindowTagList` | Implemented | Parses the basic `WA_*` tags (`WA_Left`, `WA_Top`, `WA_Width`, `WA_Height`, `WA_Title`, `WA_Flags`, `WA_IDCMP`, plus `WA_MinWidth/MinHeight/MaxWidth/MaxHeight`) and uses the `NewWindow` pointer as defaults when supplied. |
| `CloseWindow` | Implemented | Destroys the host window and frees the guest slot. |

### Window ordering and focus

| Function | Status | Notes |
|----------|--------|-------|
| `WindowToFront` | Implemented | Calls `WM_RaiseWindow` to bring the window to the top and focus it. |
| `WindowToBack` | Implemented | Calls `WM_LowerWindow` to send the window to the bottom of the z-order. |
| `ActivateWindow` | Implemented | Calls `WM_RequestFocus` to focus the window. |

### Window geometry and chrome

| Function | Status | Notes |
|----------|--------|-------|
| `MoveWindow` | Implemented | Translates the window by `dx, dy` and updates the guest `Window` position. |
| `SizeWindow` | Implemented | Resizes by closing and reopening the host window at the new size. The resulting size is clamped to the original `NewWindow` `MinWidth/MinHeight` and `MaxWidth/MaxHeight` stored in the internal slot. |
| `RefreshWindowFrame` | Implemented | Calls `WM_RepaintWindow` to request a chrome/content repaint. |
| `SetWindowTitles` | Implemented | Reads the new guest title, updates the guest `Window` title pointer, and calls `WM_SetWindowTitle` to refresh the native title bar. |

### Workbench screen

| Function | Status | Notes |
|----------|--------|-------|
| `OpenWorkbench` | Implemented | Tracks a simple Workbench-open state and returns a synthetic screen pointer. |
| `CloseWorkBench` | Implemented | Clears the Workbench-open state and returns the expected boolean. |

### Drawing helpers

| Function | Status | Notes |
|----------|--------|-------|
| `DrawBorder` | Implemented | Thin wrapper that sets the RastPort pen/mode and issues `Move`/`Draw` graphics calls for each `Border` segment. |
| `DrawImage` | Implemented | Thin wrapper that builds a temporary guest `BitMap` from the `Image` data and calls `BltBitMapRastPort` (or `BltTemplate` for 1-bitplane images). |
| `PrintIText` | Implemented | Thin wrapper that opens/sets the requested font, configures pens/mode, and calls `Move`/`Text` to render each `IntuiText` string. |

### IDCMP

| Function | Status | Notes |
|----------|--------|-------|
| `ModifyIDCMP` | Implemented | Writes the new IDCMP flags into the guest `Window`. |

## Guest data structures

The guest `Window` structure is kept AmigaOS 3.x-compatible at the offsets used by typical M68k binaries:

- `LeftEdge` / `TopEdge` / `Width` / `Height` at offsets 4, 6, 8, 10.
- `RPort` pointer at offset 50.
- Standard `WFLG_*` window flags and `IDCMP_*` IDCMP flags are defined.

## Window manager integration

The Intuition library keeps a small internal table (`IntuitionSlot`) that maps guest `Window` pointers to host WM handles. All ordering/repaint requests are forwarded to the host WM:

- `WM_RaiseWindow` — bring a window to front.
- `WM_LowerWindow` — send a window to back.
- `WM_RequestFocus` — activate a window.
- `WM_RepaintWindow` — request a chrome/content repaint.
