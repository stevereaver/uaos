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
| `OpenWindowTagList` | Implemented | Parses the full `WA_*` tag set, including geometry, title, flags, system gadgets, refresh mode, public/custom screen, and min/max bounds, and uses the `NewWindow` pointer as defaults when supplied. |
| `CloseWindow` | Implemented | Destroys the host window and frees the guest slot, plus any `UserPort`/`WindowPort` pending messages, the gadget list, the `RastPort`, and the `Window` structure. |

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

### Refresh and damage handling

| Function | Status | Notes |
|----------|--------|-------|
| `BeginRefresh` | Implemented | Marks the start of a refresh cycle for the supplied `Window`. The internal `IntuitionSlot` damage rectangle is set to the window content area. |
| `EndRefresh` | Implemented | Ends the refresh cycle; if `complete` is non-zero the damage rectangle is cleared. |

SimpleRefresh windows receive `IDCMP_REFRESHWINDOW` when the host WM repaints the window or when a resize occurs. SmartRefresh windows rely on the host WM to preserve their contents; Intuition redraws the gadgets and borders on top. The refresh mode is stored in the `IntuitionSlot` and can be changed via `WA_SimpleRefresh` / `WA_SmartRefresh` / `WA_SuperBitMap`.

### Workbench screen

| Function | Status | Notes |
|----------|--------|-------|
| `OpenWorkbench` | Implemented | Creates a real guest `Screen` structure with a `RastPort`, `WBENCHSCREEN | PUBLICSCREEN | SHOWTITLE` flags, and the public name `"Workbench"`. |
| `CloseWorkBench` | Implemented | Closes the Workbench screen, frees its `RastPort` and `Screen` memory, and releases the internal slot. |

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

### Gadgets

| Function | Status | Notes |
|----------|--------|-------|
| `AddGadget` | Implemented | Inserts a single gadget into the guest `Window.FirstGadget` list at the requested position (or at the end). |
| `AddGList` | Implemented | Inserts a linked list of `Numgad` gadgets into the window list. |
| `RemoveGadget` | Implemented | Detaches the gadget from the window list and returns its previous position. |
| `RemoveGList` | Implemented | Detaches `Numgad` gadgets starting at the given gadget. |
| `RefreshGList` | Implemented | Triggers a full WM redraw so the gadget imagery is repainted. |
| `RefreshGadgets` | Implemented | Older Intuition entry point equivalent to `RefreshGList`; triggers a full WM redraw. |
| `OnGadget` | Implemented | Clears `GFLG_DISABLED` on the gadget and redraws. |
| `OffGadget` | Implemented | Sets `GFLG_DISABLED` on the gadget and redraws. |
| `ModifyProp` / `NewModifyProp` | Implemented | Updates the `PropInfo` fields of a proportional gadget and redraws. |
| `ActivateGadget` | Implemented | Focuses the window for string gadgets; returns `TRUE` for string gadgets, `FALSE` otherwise. |

Custom gadgets are rendered as simple bevelled boxes (boolean), tracks with knobs (proportional), or input boxes (string). Integer gadgets show the numeric contents of their `StringInfo` buffer; listview gadgets render a scrollable list of items with a selected row; boolean gadgets with `GACT_TOGGLESELECT` render as checkboxes and those with a non-zero `MutualExclude` mask render as radio buttons (mutual exclusion is enforced on mouse-up). Mouse presses on a non-disabled custom gadget post `IDCMP_GADGETDOWN` and set `GFLG_SELECTED`; releases over the same gadget post `IDCMP_GADGETUP`. The internal `GetGadgetInfo(gad, info_id)` helper can read type, flags, selected state, integer value, string buffer, listview selection, or proportional pot values. The guest application is still responsible for complex imagery via the RastPort and for calling `RefreshGList()` / `RefreshGadgets()` after changing gadget visuals.

### Window/screen attribute tags

| Function | Status | Notes |
|----------|--------|-------|
| `SetWindowAttrsA` | Implemented | Full `WA_*` tag coverage: geometry, title, screen title, IDCMP, flags, system/border/backdrop/refresh flags (`WA_SizeGadget`, `WA_DragBar`, `WA_DepthGadget`, `WA_CloseGadget`, `WA_Backdrop`, `WA_ReportMouse`, `WA_Borderless`, `WA_GimmeZeroZero`, `WA_Activate`, `WA_RMBTrap`, `WA_SimpleRefresh`, `WA_SmartRefresh`, `WA_SuperBitMap`, etc.), gadgets, min/max bounds, public/custom screen, `WA_PubScreenName`, `WA_PubScreenFallBack`, `WA_InnerWidth/Height`, `WA_MouseQueue`, `WA_RptQueue`, `WA_NewLookMenus`, `WA_MenuHelp`, `WA_AmigaKey`, `WA_NotifyDepth`, `WA_HelpGroup`, `WA_TabletMessages`, and storage tags. `WA_MouseQueue` and `WA_RptQueue` are enforced by dropping the oldest queued message of the corresponding class when the limit is reached. `WA_SuperBitMap` forces the refresh mode to super-bitmap. Moves/resizes the native WM window as needed. |
| `GetWindowAttrsA` | Implemented | Reads back the corresponding live window fields, flags, and stored tag values. |
| `SetScreenAttrsA` | Implemented | Full `SA_*` tag coverage: geometry, depth, detail/block pens, title, font, type, bitmap, display ID, colors, pens, behind/quiet/autoscroll/showtitle flags, full palette, color map entries, parent, draggable/exclusive/share-pens/interleaved/like-workbench/minimize-ISG, and other storage tags. `SA_DClip` and `SA_Overscan` are used to determine the screen dimensions when opening or resizing a screen. `SA_PubSig`/`SA_PubTask` are used to signal the owning task when a screen's public status changes. Refreshes the desktop title when needed. |
| `GetScreenAttrsA` | Implemented | Reads back the corresponding live screen fields, flags, and stored tag values. |
| `GetVisualInfoA` | Implemented | Allocates a real guest `VisualInfo` structure tied to the supplied screen, including a freshly allocated `DrawInfo`. |
| `FreeVisualInfo` | Implemented | Frees the `DrawInfo` and the `VisualInfo` structure. |

`WA_*` and `SA_*` tag values, `WFLG_*` window flags, and screen type flags now match the AmigaOS 3.x definitions. Tags that don't have a live effect (e.g., `WA_Colors`, `WA_Zoom`, `WA_BackFill`, `WA_HelpGroup`, `WA_HelpGroupWindow`, `SA_Parent`, `SA_BackFill`, `SA_FullPalette`) are stored in the internal `IntuitionSlot` / `ScreenSlot` so they can be read back with `GetWindowAttrsA` / `GetScreenAttrsA`. The most actionable tags (mouse/repeat queues, super-bitmap refresh mode, display clip/overscan, and public-screen notification signals) are now enforced.

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
| `FreeSysRequest` | Implemented | Closes the requester window, releases the associated signal bit, and frees the requester guest window, `RastPort`, and any IDCMP resources. |
| `EasyRequestArgs` | Implemented | Parses an `EasyStruct`, builds a native WM requester with the requested buttons (separated by `\|` in `es_GadgetFormat`), waits for a click, and returns the gadget index (0 for the rightmost choice). |
| `EasyRequest` | Implemented | Varargs wrapper for `EasyRequestArgs()`; the compiler stub packages the varargs and the shared LVO entry forwards to the same requester implementation. |
| `DisplayAlert` | Implemented | Displays a centred alert window with a black background and amber/red border. Parses the Amiga alert-message substrings (2-byte X, 1-byte Y, null-terminated text, continuation byte) and waits for a left mouse button (`TRUE`) or right button/close (`FALSE`). `DEADEND_ALERT` displays immediately and returns `FALSE`. |
| `TimedDisplayAlert` | Implemented | Equivalent to `DisplayAlert()` but with a video-frame timeout; if the user does not respond before the timeout expires the alert returns `FALSE`. |
| `InitRequester` | Implemented | Zeroes the guest `Requester` structure. |
| `Request` | Implemented | Builds a synchronous modal requester from `req_Text` and `req_Gadgets`, waits for a button click, and returns `TRUE` for any non-rightmost gadget and `FALSE` for the rightmost gadget. |
| `EndRequest` | Implemented | Ensures the active requester window is closed. |
| `SysReqHandler` | Implemented | Polls the requester window's `UserPort` and returns the class of the next pending `IntuiMessage`. If `waitInput` is non-zero, it yields the CPU until a message arrives. |

Requesters are rendered using the native WM: a grey window with a title bar, body text, and one or more buttons. The calling M68k task blocks on an `AllocSignal()` bit until the user clicks a button or the close gadget; the result is then returned and the requester is closed. Alerts use a separate alert slot and a dedicated draw function with a black background and coloured border. `SysReqHandler()` allows asynchronous requester handling by returning the IDCMP class of the next queued message.

### Screens

| Function | Status | Notes |
|----------|--------|-------|
| `OpenScreen` | Implemented | Parses a `NewScreen` and the full `SA_*` tag set (geometry, depth, detail/block pens, title, font, type, bitmap, display ID, colors, overscan, behind/quiet/autoscroll/showtitle flags, full palette, etc.) and allocates a guest `Screen` structure. Defaults to the framebuffer dimensions. The active screen title is displayed in the desktop menu bar. |
| `OpenScreenTagList` | Implemented | Same as `OpenScreen` but merges `SA_*` tags from the supplied tag list. |
| `CloseScreen` | Implemented | Releases the guest `Screen` slot and clears the desktop title if the closed screen was active. |
| `MoveScreen` | Implemented | Updates the guest `Screen` `LeftEdge`/`TopEdge` fields. |
| `ScreenToFront` | Implemented | Marks the screen as the active screen and refreshes the desktop title. |
| `ScreenToBack` | Implemented | Marks the screen as behind and picks another active screen for the title. |
| `ScreenDepth` | Implemented | V39 depth-arrangement: `SDEPTH_TOFRONT`/`SDEPTH_TOBACK` adjust the front/back flag. |
| `ScreenPosition` | Implemented | V39 screen positioning: `SPOS_RELATIVE` moves by `(x1,y1)`, `SPOS_ABSOLUTE` sets position, `SPOS_MAKEVISIBLE` clamps a rectangle into view. |
| `ShowTitle` | Implemented | Toggles the `SHOWTITLE` flag and shows/hides the screen title in the desktop menu bar. |

Screens are mapped to the single UAOS desktop. Multiple screens are tracked in a small internal table; the frontmost screen with `SHOWTITLE` set controls the title displayed in the native desktop menu bar.

### Public screens and visitor windows

| Function | Status | Notes |
|----------|--------|-------|
| `LockPubScreen` | Implemented | Returns a named public screen by `SA_PubName`, or the default/frontmost screen when name is `NULL`. The screen's lock count is incremented. |
| `UnlockPubScreen` | Implemented | Decrements the screen's lock count. |
| `LockPubScreenList` | Implemented | Returns a real guest `List` containing a node for every active public screen. Each node stores the screen's public name and guest `Screen*` pointer. |
| `UnlockPubScreenList` | Implemented | Walks the list, frees all nodes and their name strings, and frees the list header. |
| `OpenWindowTagList` visitor | Implemented | Supports `WA_PubScreen`, `WA_PubScreenName`, and `WA_PubScreenFallBack`. Resolves the target screen and stores it in `Window.WScreen`; windows are positioned relative to the public screen. |

Public screens are registered via `SA_PubName` when opening a screen. `OpenWindowTagList()` now treats windows without an explicit screen as visitor windows on the default public screen, and visitor windows on named screens fall back to the default screen when `WA_PubScreenFallBack` is set. When `SA_PubSig` and `SA_PubTask` are set, the specified task is signalled both when the screen is opened as public and whenever `PubScreenStatus()` changes its public/private status.

### Pointer shapes

| Function | Status | Notes |
|----------|--------|-------|
| `SetPointer` | Implemented | Reads a 16-bit-wide Amiga sprite definition from guest memory and installs it as a custom native cursor. Up to 16×16 pixels; sprite planes are mapped to body/shadow colours. |
| `ClearPointer` | Implemented | Restores the default native arrow cursor. |
| `SetWindowPointerA` | Implemented | Supports `WA_BusyPointer` (TRUE installs a busy/hourglass cursor, FALSE restores the default), `WA_Pointer` (renders the custom pointer), and `WA_PointerDelay` (accepted, currently changes immediately). A tag-list with no tags clears the custom pointer. |

The native cursor subsystem gained custom-sprite support: a per-window custom pointer can replace the default arrow, and a busy pointer can be activated when the active window requests it. `WA_Pointer` objects are rendered by heuristically detecting a SetPointer-style sprite buffer, a `struct BitMap`, or a `pointerclass` BOOPSI object whose first valid pointer is a BitMap. `SetPointer()` now correctly skips the Amiga sprite reserved words and passes the caller's x/y hotspot offsets to the cursor renderer.

### Preferences / defaults

| Function | Status | Notes |
|----------|--------|-------|
| `GetDefPrefs` | Implemented | Fills the supplied buffer with a static default `Preferences` structure and returns the buffer pointer. |
| `GetPrefs` | Implemented | Fills the supplied buffer with the current `Preferences` snapshot and returns the buffer pointer. |
| `SetPrefs` | Implemented | Copies the supplied `Preferences` bytes into the internal snapshot and immediately applies the live fields: the four Workbench colours (`color0`–`color3`) are mapped to the runtime `WB_*` palette, the font height and Workbench dimensions are stored as globals, and the desktop is repainted. |
| `LockGUIPrefs` | Implemented | Returns a dummy lock pointer. |
| `UnlockGUIPrefs` | Implemented | No-op. |

A full `Preferences` structure layout (`struct Preferences` offsets and `PREF_SIZE`) is defined in `intuition_lib.h`. Defaults include a Topaz 8 font, 9600 baud, 640×200×2 Workbench dimensions, and a basic Workbench palette.

The Workbench palette (`WB_GREY`, `WB_BLUE`, `WB_LIGHT_BLUE`, `WB_DARK_GREY`, etc.) is now stored in runtime variables rather than compile-time constants, so colour changes submitted via `SetPrefs()` propagate through the desktop, windows, and gadgets. Font and dimension fields are captured in globals for use by screen/window creation code.

### Display-mode queries

| Function | Status | Notes |
|----------|--------|-------|
| `QueryOverscan` | Implemented | Returns a `Rectangle` covering the full framebuffer (0,0 to width-1,height-1). |
| `GetDisplayInfoData` | Implemented | Intuition wrapper that forwards the call to `graphics.library/GetDisplayInfoData`. |
| `NextDisplayInfo` | Implemented | Intuition wrapper that forwards the call to `graphics.library/NextDisplayInfo`. |

The underlying `graphics.library` display database already provides `FindDisplayInfo`, `NextDisplayInfo`, `GetDisplayInfoData`, `ModeNotAvailable`, and `BestModeIDA`; the new Intuition wrappers delegate to those slots so callers can use either library entry point.

### Missing utility functions

| Function | Status | Notes |
|----------|--------|-------|
| `CurrentTime` | Implemented | Reads the host RTC and PIT ticks and writes real seconds (AmigaOS 1978 epoch) and microseconds into the guest buffers. |
| `DoubleClick` | Implemented | Returns TRUE if the two timestamps are within 500 ms of each other. |
| `ReportMouse` | Implemented | Sets or clears the `WFLG_REPORTMOUSE` flag in the guest `Window.Flags`. |
| `DisplayBeep` | Implemented | Briefly flashes the desktop backdrop to a bright colour (50 ms) to provide visual feedback. |
| `InitRequester` | Implemented | Zeroes the guest `Requester` structure. |
| `EndRequest` | Implemented | Ensures any active requester window is closed. |
| `Request` | Implemented | Builds a synchronous modal requester from `req_Text` and `req_Gadgets`; waits for a click and returns `TRUE`/`FALSE`. |
| `ViewAddress` | Implemented | Returns NULL. |
| `ViewPortAddress` | Implemented | Returns a pointer to the screen's `ViewPort` (uses `Window.WScreen`). |
| `GetScreenData` | Implemented | Copies up to 256 bytes from the appropriate screen: `CUSTOMSCREEN` (type 0) uses the supplied `Screen` pointer; `WBENCHSCREEN` uses the Workbench screen; `PUBLICSCREEN` uses the default public screen. Unknown types zero the buffer. |
| `NextPubScreen` | Implemented | Walks the public-screen list and returns the next public screen, writing its name into the supplied buffer. |
| `SetDefaultPubScreen` | Implemented | Stores the name in an internal global. |
| `GetDefaultPubScreen` | Implemented | Copies the current default public screen name into the supplied buffer and returns the screen pointer in D0. |
| `PubScreenStatus` | Implemented | Toggles the public/private status of a screen; `PS_PUBLIC` adds it to the public screen list, `PS_PRIVATE` removes it. |
| `LockIBase` / `UnlockIBase` | Implemented | Real recursive lock: `LockIBase` increments and returns a counter; `UnlockIBase` decrements it. |
| `ShowWindow` / `HideWindow` | Implemented | Raise / lower the native WM window. |
| `WindowLimits` | Implemented | Stores the min/max dimensions in the `IntuitionSlot`. |
| `ChangeWindowBox` | Implemented | Updates the guest `Window` position/size and moves the native WM window. |
| `GetScreenDrawInfo` / `FreeScreenDrawInfo` | Implemented | Allocates a real `DrawInfo` matching the screen's font and detail/block pens, and frees it on release. |

### Menu strips

| Function | Status | Notes |
|----------|--------|-------|
| `SetMenuStrip` | Implemented | Stores the supplied `Menu*` in the guest `Window.MenuStrip` field. |
| `ClearMenuStrip` | Implemented | Clears `Window.MenuStrip`. |
| `ResetMenuStrip` | Implemented | Same as `SetMenuStrip`; stores the new `Menu*` in `Window.MenuStrip`. |
| `ItemAddress` | Implemented | Parses a classic 16-bit `menuNumber` into menu/item/sub-item indices and walks the linked `Menu`/`MenuItem` lists to return the matching `MenuItem*`. |
| `OnMenu` / `OffMenu` | Implemented | Enable or disable a menu, item, or sub-item addressed by a packed `menuNumber`. |

Menu structures are kept in guest memory; the Intuition library only maintains the `Window.MenuStrip` pointer for the caller. `ItemAddress`, `OnMenu`, and `OffMenu` follow `Menu.NextMenu`, `Menu.FirstItem`, `MenuItem.NextItem`, and `MenuItem.SubItem` chains.

The desktop menu bar now renders the active guest window's `MenuStrip`. When a guest window is focused, its `Menu` titles are parsed and drawn in the menu bar; opening a menu and selecting an enabled item posts an `IDCMP_MENUPICK` message with the classic packed menu number `(menu | item << 5 | sub << 11)`. Submenus are parsed from `MenuItem.SubItem` and drawn as cascading menus. Items with `CHECKIT` show a check box that is filled when checked; `MENUTOGGLE` items toggle their checked state on selection, while plain `CHECKIT` items are set. `COMMSEQ` command-key shortcuts are displayed next to the item label. Disabled items (`ITEMENABLED` clear) are greyed out and cannot be selected. When no guest window has a menu strip, the desktop falls back to the built-in Workbench menu.

Mouse and repeat queues are enforced: `WA_MouseQueue` limits pending `IDCMP_MOUSEMOVE` messages, and `WA_RptQueue` limits pending `IDCMP_MOUSEBUTTONS` messages. When a new message would exceed the queue limit, the oldest queued message of that class is removed before the new one is added. A limit of 0 means no restriction.

## Guest data structures

The guest `Window` structure is kept AmigaOS 3.x-compatible at the offsets used by typical M68k binaries:

- `LeftEdge` / `TopEdge` / `Width` / `Height` at offsets 4, 6, 8, 10.
- `RPort` pointer at offset 50.
- `BorderLeft` / `BorderTop` / `BorderRight` / `BorderBottom` at offsets 54, 55, 56, 57.
- Standard `WFLG_*` window flags and `IDCMP_*` IDCMP flags are defined.

The guest `Screen` structure now includes `DetailPen`/`BlockPen` (offsets 70/71), `RastPort` pointer (offset 80), `Depth` (offset 84), `BitMap` (offset 88), `DisplayID` (offset 92), and `Colors` (offset 96). The `DrawInfo` structure is allocated with 16 drawing pens, the screen's font pointer, and default depth/resolution fields.

Intuition uses a dedicated 64 KiB guest heap (below the stack) for small allocations such as `Window`, `Screen`, `RastPort`, `MsgPort`, `IntuiMessage`, and `DrawInfo` structures. The heap is now a free-list allocator with block headers, so `CloseWindow`, `FreeSysRequest`, `CloseWorkBench`, and `FreeScreenDrawInfo` reclaim the memory they allocate.

The `IntuitionSlot` and `ScreenSlot` host structures cache tag values that have no direct live effect (e.g., `WA_Colors`, `WA_Zoom`, `SA_BackFill`, `SA_Parent`) so that `GetWindowAttrsA` and `GetScreenAttrsA` can return the values supplied by the guest.

`GimmeZeroZero` windows store their border sizes in the `IntuitionSlot` and report content-relative mouse coordinates in `IDCMP` messages. The window `RastPort` stores the guest `Window*` pointer in its `Layer` field (`RP_OFF_LAYER`), and `graphics.library` adds the window's screen position plus the GimmeZeroZero border offset to all framebuffer drawing operations so that guest drawing commands are relative to the window content area.

## Window manager integration

The Intuition library keeps a small internal table (`IntuitionSlot`) that maps guest `Window` pointers to host WM handles. All ordering/repaint requests are forwarded to the host WM:

- `WM_RaiseWindow` — bring a window to front.
- `WM_LowerWindow` — send a window to back.
- `WM_RequestFocus` — activate a window.
- `WM_RepaintWindow` — request a chrome/content repaint.

## LVO offsets

Intuition LVO offsets in `emulation/uaos_m68k_glue.c` match the AmigaOS 3.x `intuition_lib.fd` file. Recent additions and corrections include `OnMenu` (-114), `OffMenu` (-108), `ViewAddress` (-126), `ViewPortAddress` (-132), `GetScreenData` (-306), `LockIBase` (-294), `UnlockIBase` (-300), `LockPubScreen` (-384), `UnlockPubScreen` (-390), `LockPubScreenList` (-396), `UnlockPubScreenList` (-402), `NextPubScreen` (-408), `SetDefaultPubScreen` (-420), `PubScreenStatus` (-426), `GetDefaultPubScreen` (-432), `SysReqHandler` (-450), `GetVisualInfoA` (-630), and `FreeVisualInfo` (-636).
