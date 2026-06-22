---
type: Kernel Library
title: intuition.library
description: UAOS native implementation of the AmigaOS intuition.library for emulated M68k tasks.
resource: /kernel/exec/intuition_lib.c
tags: [intuition, library, m68k, thunking, window, wm]
timestamp: 2026-06-22T17:00:00Z
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
| `ModifyIDCMP` | Implemented | Writes the new IDCMP flags into the guest `Window` and allocates a `UserPort`/`WindowPort` pair if one is not present and IDCMP is now wanted. |

### IDCMP event injection

| Event class | Status | Notes |
|-------------|--------|-------|
| `IDCMP_MOUSEBUTTONS` | Implemented | Mouse button press/release on the window client area is converted to window-relative coordinates and posted to the `UserPort`. |
| `IDCMP_MOUSEMOVE` | Implemented | Mouse moves over the focused window are posted with window-relative coordinates. |
| `IDCMP_RAWKEY` / `IDCMP_VANILLAKEY` | Implemented | WM keystrokes are posted as raw/vanilla key events. |
| `IDCMP_CLOSEWINDOW` | Implemented | Clicking the window close gadget is vetoed at the WM level and posted as an `IDCMP_CLOSEWINDOW` message; the guest must call `CloseWindow()` to actually close it. |
| `IDCMP_GADGETDOWN` / `IDCMP_GADGETUP` | Implemented | The WM's existing system gadgets (close, drag, depth, size) are exposed as guest `Gadget` structures and linked into `Window.FirstGadget`. Pressing and releasing a system gadget posts the corresponding `IDCMP_GADGETDOWN`/`IDCMP_GADGETUP` message with the gadget pointer in `IAddress`. |
| `IDCMP_NEWSIZE` | Implemented | Posted when the window is resized via the resize grip or via `SizeWindow()`. |
| `IDCMP_ACTIVEWINDOW` / `IDCMP_INACTIVEWINDOW` | Implemented | Focus changes (raise/lower/close/click-to-focus) are posted to the `UserPort`. |

When a window is opened with non-zero IDCMP flags, Intuition allocates a guest `MsgPort` for `UserPort`, a reply `MsgPort` for `WindowPort`, and an `AllocSignal()` signal bit. WM events are packaged as guest `IntuiMessage` structures and queued on the `UserPort`; the owning M68k task is then signalled so a guest `WaitPort()`/`GetMsg()` loop can receive them. The guest can reply messages via `ReplyMsg()`; replies are returned to the guest `WindowPort`.

### Requesters

| Function | Status | Notes |
|----------|--------|-------|
| `AutoRequest` | Implemented | Builds a native WM requester with the supplied body text and positive/negative gadgets, waits for a button click, and returns `TRUE`/`FALSE`. |
| `BuildSysRequest` | Implemented | Builds a native WM requester window and returns a guest `Window*` handle; the caller can wait on the requester IDCMP port. |
| `FreeSysRequest` | Implemented | Closes the requester window and releases the associated signal bit. |
| `EasyRequestArgs` | Implemented | Parses an `EasyStruct`, builds a native WM requester with the requested buttons (separated by `\|` in `es_GadgetFormat`), waits for a click, and returns the gadget index (0 for the rightmost choice). |

Requesters are rendered using the native WM: a grey window with a title bar, body text, and one or more buttons. The calling M68k task blocks on an `AllocSignal()` bit until the user clicks a button or the close gadget; the result is then returned and the requester is closed.

### Screens

| Function | Status | Notes |
|----------|--------|-------|
| `OpenScreen` | Implemented | Parses a `NewScreen` and allocates a guest `Screen` structure. Defaults to the framebuffer dimensions. The active screen title is displayed in the desktop menu bar. |
| `OpenScreenTagList` | Implemented | Same as `OpenScreen` but merges `SA_*` tags from the supplied tag list. |
| `CloseScreen` | Implemented | Releases the guest `Screen` slot and clears the desktop title if the closed screen was active. |
| `MoveScreen` | Implemented | Updates the guest `Screen` `LeftEdge`/`TopEdge` fields. |
| `ScreenToFront` | Implemented | Marks the screen as the active screen and refreshes the desktop title. |
| `ScreenToBack` | Implemented | Marks the screen as behind and picks another active screen for the title. |
| `ShowTitle` | Implemented | Toggles the `SHOWTITLE` flag and shows/hides the screen title in the desktop menu bar. |

Screens are mapped to the single UAOS desktop. Multiple screens are tracked in a small internal table; the frontmost screen with `SHOWTITLE` set controls the title displayed in the native desktop menu bar.

### Public screens and visitor windows

| Function | Status | Notes |
|----------|--------|-------|
| `LockPubScreen` | Implemented | Returns a named public screen by `SA_PubName`, or the default/frontmost screen when name is `NULL`. The screen's lock count is incremented. |
| `UnlockPubScreen` | Implemented | Decrements the screen's lock count. |
| `LockPubScreenList` | Implemented | Returns a minimal guest `List` structure. |
| `UnlockPubScreenList` | Implemented | No-op. |
| `OpenWindowTagList` visitor | Implemented | Supports `WA_PubScreen`, `WA_PubScreenName`, and `WA_PubScreenFallBack`. Resolves the target screen and stores it in `Window.WScreen`; windows are positioned relative to the public screen. |

Public screens are registered via `SA_PubName` when opening a screen. `OpenWindowTagList()` now treats windows without an explicit screen as visitor windows on the default public screen, and visitor windows on named screens fall back to the default screen when `WA_PubScreenFallBack` is set.

### Pointer shapes

| Function | Status | Notes |
|----------|--------|-------|
| `SetPointer` | Implemented | Reads a 16-bit-wide Amiga sprite definition from guest memory and installs it as a custom native cursor. Up to 16×16 pixels; sprite planes are mapped to body/shadow colours. |
| `ClearPointer` | Implemented | Restores the default native arrow cursor. |
| `SetWindowPointerA` | Implemented | Supports `WA_BusyPointer` (TRUE installs a busy/hourglass cursor, FALSE restores the default) and a tag-list with no tags (also clears the custom pointer). `WA_Pointer` custom objects are accepted but not yet rendered. |

The native cursor subsystem gained custom-sprite support: a per-window custom pointer can replace the default arrow, and a busy pointer can be activated when the active window requests it.

### Preferences / defaults

| Function | Status | Notes |
|----------|--------|-------|
| `GetDefPrefs` | Implemented | Fills the supplied buffer with a static default `Preferences` structure and returns the buffer pointer. |
| `GetPrefs` | Implemented | Fills the supplied buffer with the current `Preferences` snapshot and returns the buffer pointer. |
| `SetPrefs` | Implemented | Copies the supplied `Preferences` bytes into the internal snapshot; most fields are currently advisory only. |
| `LockGUIPrefs` | Implemented | Returns a dummy lock pointer. |
| `UnlockGUIPrefs` | Implemented | No-op. |

A full `Preferences` structure layout (`struct Preferences` offsets and `PREF_SIZE`) is defined in `intuition_lib.h`. Defaults include a Topaz 8 font, 9600 baud, 640×200×2 Workbench dimensions, and a basic Workbench palette.

### Menu strips

| Function | Status | Notes |
|----------|--------|-------|
| `SetMenuStrip` | Implemented | Stores the supplied `Menu*` in the guest `Window.MenuStrip` field. |
| `ClearMenuStrip` | Implemented | Clears `Window.MenuStrip`. |
| `ResetMenuStrip` | Implemented | Same as `SetMenuStrip`; stores the new `Menu*` in `Window.MenuStrip`. |
| `ItemAddress` | Implemented | Parses a classic 16-bit `menuNumber` into menu/item/sub-item indices and walks the linked `Menu`/`MenuItem` lists to return the matching `MenuItem*`. |

Menu structures are kept in guest memory; the Intuition library only maintains the `Window.MenuStrip` pointer for the caller. `ItemAddress` follows `Menu.NextMenu`, `Menu.FirstItem`, `MenuItem.NextItem`, and `MenuItem.SubItem` chains.

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
