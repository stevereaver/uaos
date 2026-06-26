---
type: Kernel Library
title: intuition.library
description: UAOS native implementation of the AmigaOS intuition.library for emulated M68k tasks.
resource: /kernel/exec/intuition_lib.c
tags: [intuition, library, m68k, thunking, window, wm]
timestamp: 2026-06-24T17:00:00Z
---

# intuition.library

`intuition.library` provides the classic AmigaOS windowing API to emulated M68k tasks. The UAOS implementation is a native thunk layer that translates guest `OpenWindow`/`CloseWindow` and related calls into the host window manager API.

## Key files

- `kernel/exec/intuition_lib.c` — native implementations and function table.
- `kernel/exec/intuition_lib.h` — minimal guest `Window` / `NewWindow` structures and flags, plus BOOPSI class/object offsets and attribute/method IDs.
- `kernel/exec/boopsi_builtin.c` — native dispatchers for the built-in BOOPSI classes.
- `kernel/exec/boopsi_builtin.h` — built-in class registration prototype.
- `kernel/display/wm.h` — host window manager API.
- `emulation/uaos_m68k_glue.c` — M68k LVO stub installation and dispatch.

## Implementation status

### Window lifecycle

| Function | Status | Notes |
|----------|--------|-------|
| `OpenWindow` | Implemented | Translates a guest `NewWindow` into a host WM window and allocates a guest `Window` + `RastPort`. Width/height are clamped to the `NewWindow` `MinWidth/MinHeight` and `MaxWidth/MaxHeight` bounds. |
| `OpenWindowTagList` | Implemented | Parses the full `WA_*` tag set, including geometry, title, flags, system gadgets, refresh mode, public/custom screen, min/max bounds, and stored miscellaneous tags (`WA_Colors`, `WA_Checkmark`, `WA_AmigaKey`, `WA_MenuHelp`, `WA_TabletMessages`, `WA_AutoAdjust`, `WA_NotifyDepth`, `WA_PointerDelay`). Uses the `NewWindow` pointer as defaults when supplied, including `NewWindow.CheckMark` as the default for `WA_Checkmark`. `WA_AutoAdjust` clamps the initial window position to the containing screen; `WA_MenuHelp` and `WA_TabletMessages` set the `IDCMP_MENUHELP` and `IDCMP_TABLET` flags; `WA_NotifyDepth` enables `IDCMP_NEWSIZE` on z-order changes; `WA_PointerDelay` is applied to `SetWindowPointerA` pointer changes. |
| `CloseWindow` | Implemented | Destroys the host window and frees the guest slot, plus any `UserPort`/`WindowPort` pending messages, the gadget list, the `RastPort`, and the `Window` structure. |

### Window ordering and focus

| Function | Status | Notes |
|----------|--------|-------|
| `WindowToFront` | Implemented | Calls `WM_RaiseWindow` to bring the window to the top and focus it. |
| `WindowToBack` | Implemented | Calls `WM_LowerWindow` to send the window to the bottom of the z-order. |
| `ActivateWindow` | Implemented | Calls `WM_RequestFocus` to focus the window. |
| `MoveWindowInFrontOf` | Implemented | Repositions the source window directly in front of the `behind` window in the WM's back-to-front z-order array. If `behind` is `NULL` (or an invalid window), the source window is raised to the top. The focused window is left unchanged, and the scene is redrawn. The WM helper now correctly inserts the source window at `behind_pos + 1` even when `behind` is the frontmost window, instead of falling back to a simple raise. |

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

SimpleRefresh windows receive `IDCMP_REFRESHWINDOW` when the host WM repaints the window or when a resize occurs. SmartRefresh windows rely on the host WM to preserve their contents; Intuition redraws the gadgets and borders on top. The refresh mode is stored in the `IntuitionSlot` and can be changed via `WA_SimpleRefresh` / `WA_SmartRefresh` / `WA_SuperBitMap`. During `BeginRefresh`/`EndRefresh` the `IntuitionSlot` damage rectangle is set; the host WM draw callback now clips gadget rendering to that rectangle, so host-side gadgets are only redrawn inside the damaged area.

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
| `SetEditHook` | Implemented | Stores the new edit hook in the gadget's `UserData` and returns the previous hook in D0. |
| `ObtainGIRPort` / `ReleaseGIRPort` | Implemented | Allocates a temporary guest `RastPort` for gadget rendering and frees it. The returned port is a minimal `RastPort` suitable for the host's rendering helpers. |

Custom gadgets are rendered as simple bevelled boxes (boolean), tracks with knobs (proportional), or input boxes (string). Integer gadgets show the numeric contents of their `StringInfo` buffer; listview gadgets render a scrollable list of items with a selected row; boolean gadgets with `GACT_TOGGLESELECT` render as checkboxes and those with a non-zero `MutualExclude` mask render as radio buttons (mutual exclusion is enforced on mouse-up). Mouse presses on a non-disabled custom gadget post `IDCMP_GADGETDOWN` and set `GFLG_SELECTED`; releases over the same gadget post `IDCMP_GADGETUP`. String and integer gadgets are now editable: clicking a string gadget activates it and places a cursor at the click position; subsequent keystrokes insert/delete characters, move the cursor (arrows), jump to the start/end (up/down), and are committed with Return/Tab (which posts `IDCMP_GADGETUP` and deactivates the gadget). Shift+arrow keys or Shift+click extend a selection; Ctrl+A selects all; typing or backspace/delete replaces the selection. The selected range is highlighted in blue. Focus loss or clicking outside the gadget also deactivates it.

Proportional gadgets (`GTYP_PROPGADGET`) are now interactive: clicking inside the knob and dragging updates the `HorizPot`/`VertPot` values in real time; clicking on the track outside the knob jumps the knob to the click position. The drag is committed on mouse-up with an `IDCMP_GADGETUP` message. Listview gadgets (`GTYP_LISTVIEW`) support their own scrollbar: dragging the scrollbar thumb scrolls the visible items, and clicking an item selects it. If the listview's `MultiSelect` flag is non-zero, holding Shift or Ctrl while clicking toggles the item's selection using a 32-bit `SelectedMask` bitmask; `GetGadgetInfo(gad, 5)` returns this bitmask in multi-select mode or the single selected index otherwise. Integer gadgets (`GTYP_INTGADGET`) validate their value on commit: the buffer is parsed as a signed integer, clamped to `Min`/`Max` bounds stored at `StringInfo` offsets 20 and 24, and rewritten with the validated value. If both bounds are zero the default range is -32768 to 32767. The internal `GetGadgetInfo(gad, info_id)` helper can read type, flags, selected state, integer value, string buffer, listview selection/mask, or proportional pot values. The guest application is still responsible for complex imagery via the RastPort and for calling `RefreshGList()` / `RefreshGadgets()` after changing gadget visuals.

### BOOPSI dispatch

| Function | Status | Notes |
|----------|--------|-------|
| `NewObjectA` | Implemented | Allocates a BOOPSI object header plus instance data for the target class hierarchy, sets the object class, and dispatches `OM_NEW` to the class dispatcher. Returns the object pointer or `NULL` on failure. |
| `DisposeObject` | Implemented | Dispatches `OM_DISPOSE` to the object's class dispatcher, then frees the object memory. |
| `SetAttrsA` | Implemented | Builds an `opSet` message and dispatches `OM_SET` to the object's class. |
| `GetAttr` | Implemented | Builds an `opGet` message and dispatches `OM_GET`; the dispatcher writes the attribute value into the supplied storage pointer. |
| `GetAttrsA` | Implemented | Iterates the supplied tag list and dispatches `OM_GET` for each attribute. Returns success if every attribute was resolved. |
| `SetSuperAttrsA` | Implemented | Dispatches `OM_SET` to the superclass of the class currently handling the method. |
| `DoMethodA` | Implemented | Dispatches an arbitrary method to the object's class dispatcher. |
| `DoSuperMethodA` | Implemented | Dispatches an arbitrary method to the superclass of the currently active class during a dispatcher call. |
| `CoerceMethodA` | Implemented | Dispatches an arbitrary method to a specific class's dispatcher as if the object were of that class. |
| `DoGadgetMethodA` | Implemented | Dispatches a gadget method to the BOOPSI gadget's class dispatcher. |
| `SetGadgetAttrsA` | Implemented | Gadget-specific `OM_SET` dispatch. The window pointer is passed as a minimal `GadgetInfo` substitute to the generic `SetAttrsA` dispatcher. |
| `MakeClass` | Implemented | Allocates and initializes an `IClass` structure, computes `cl_InstOffset` from the superclass, and returns the class pointer. |
| `FreeClass` | Implemented | Removes the class from the public registry and frees the `IClass` structure. |
| `AddClass` | Implemented | Adds an `IClass` to the internal public-class registry. |
| `RemoveClass` | Implemented | Removes an `IClass` from the public-class registry. |
| `NextObject` | Implemented | Walks the linked list of objects via the `ln_Succ` field in the `_Object` header. |

BOOPSI dispatch uses the host `UAOS_InvokeM68kHook()` helper to call the guest class dispatcher hook with the AmigaOS convention `A0 = IClass`, `A2 = Object`, `A1 = Msg`. The dispatcher is invoked for `OM_NEW`, `OM_DISPOSE`, `OM_SET`, `OM_GET`, and any class-specific or gadget method. `DoSuperMethodA` and `SetSuperAttrsA` determine the superclass from the class that is currently on the dispatch stack, so nested dispatcher calls work correctly. Object memory layout follows the AmigaOS model: the `_Object` header (12 bytes) precedes the instance data, and the object pointer returned by `NewObjectA` points to the start of the instance data, with the class stored at offset -12. Classes are matched by their 32-bit `cl_ID` value in the public registry.

### Non-A varargs wrappers

| Function | Status | Notes |
|----------|--------|-------|
| `NewObject` | Implemented | Builds a `TagItem` array from the varargs on the guest stack and calls `NewObjectA`. |
| `SetAttrs` | Implemented | Builds a `TagItem` array from the varargs on the guest stack and calls `SetAttrsA`. |
| `GetAttrs` | Implemented | Builds a `TagItem` array from the varargs on the guest stack and calls `GetAttrsA`. |
| `DoMethod` | Implemented | Builds a 12-byte method message from the method ID and the first two varargs on the guest stack, then calls `DoMethodA`. |
| `DoSuperMethod` | Implemented | Same as `DoMethod` but calls `DoSuperMethodA`. |
| `CoerceMethod` | Implemented | Builds a 12-byte method message from the method ID and the first two varargs on the guest stack, then calls `CoerceMethodA`. |
| `SetGadgetAttrs` | Implemented | Builds a `TagItem` array from the varargs on the guest stack and calls `SetGadgetAttrsA`. |
| `SetSuperAttrs` | Implemented | Builds a `TagItem` array from the varargs on the guest stack and calls `SetSuperAttrsA` with `GadgetInfo` set to `NULL`. |
| `SetWindowPointer` | Implemented | Builds a `TagItem` array from the varargs on the guest stack and calls `SetWindowPointerA`. |
| `OpenWindowTags` | Implemented | Builds a `TagItem` array from the varargs on the guest stack and calls `OpenWindowTagList`. |
| `OpenScreenTags` | Implemented | Builds a `TagItem` array from the varargs on the guest stack and calls `OpenScreenTagList`. |
| `DoGadgetMethod` | Implemented | Builds a 12-byte method message from the method ID and the first two varargs on the guest stack, then calls `DoGadgetMethodA`. |

The varargs wrappers read tag/value pairs from the guest stack starting at the return address + 4, up to `TAG_DONE` or a hard limit of 32 pairs. They allocate the resulting `TagItem` array on the guest stack, invoke the corresponding A-suffix LVO, and restore the stack pointer. Method wrappers use a fixed 12-byte message (`MethodID` + two parameters) which covers the common BOOPSI message sizes; larger method-specific messages must still be dispatched through the A-suffix variants.

### Help control, screen notify, and singular attribute calls

| Function | Status | Notes |
|----------|--------|-------|
| `HelpControl` | Implemented | Stores the `HC_GADGETHELP` enable/disable state in the window's `IntuitionSlot`. |
| `StartScreenNotifyTagList` | Stub | Always returns `NULL`; no live notification channel is implemented. |
| `EndScreenNotify` | Stub | Always returns `TRUE`. |
| `GetWindowAttr` | Implemented | Builds a one-tag `TagItem` list from the supplied `attrID` and `storage` pointer, then calls `GetWindowAttrsA`. |
| `SetWindowAttr` | Implemented | Builds a one-tag `TagItem` list from the supplied `attrID` and `data` value, then calls `SetWindowAttrsA`. |
| `GetScreenAttr` | Implemented | Builds a one-tag `TagItem` list from the supplied `attrID` and `storage` pointer, then calls `GetScreenAttrsA`. |
| `SetScreenAttr` | Implemented | Builds a one-tag `TagItem` list from the supplied `attrID` and `data` value, then calls `SetScreenAttrsA`. |

### Window/screen attribute tags

| Function | Status | Notes |
|----------|--------|-------|
| `SetWindowAttrsA` | Implemented | Full `WA_*` tag coverage: geometry, title, screen title, IDCMP, flags, system/border/backdrop/refresh flags (`WA_SizeGadget`, `WA_DragBar`, `WA_DepthGadget`, `WA_CloseGadget`, `WA_Backdrop`, `WA_ReportMouse`, `WA_Borderless`, `WA_GimmeZeroZero`, `WA_Activate`, `WA_RMBTrap`, `WA_SimpleRefresh`, `WA_SmartRefresh`, `WA_SuperBitMap`, etc.), gadgets, min/max bounds, public/custom screen, `WA_PubScreenName`, `WA_PubScreenFallBack`, `WA_InnerWidth/Height`, `WA_MouseQueue`, `WA_RptQueue`, `WA_NewLookMenus`, `WA_MenuHelp`, `WA_AmigaKey`, `WA_NotifyDepth`, `WA_HelpGroup`, `WA_TabletMessages`, `WA_Colors`, `WA_Checkmark`, `WA_PointerDelay`, and storage tags. `WA_MouseQueue` and `WA_RptQueue` are enforced by dropping the oldest queued message of the corresponding class when the limit is reached. `WA_SuperBitMap` now updates the window `RastPort` bitmap and redraws the window from the backing `BitMap`. `WA_BackFill` is stored; if the value is a real guest `Hook` pointer it is invoked during WM redraws with A0=Hook, A2=window `RastPort`, A1=window-client `Rectangle`, otherwise values below 256 are still treated as a pen-index fallback. `WA_AutoAdjust` clamps the window position to the containing screen when set at open time or via `SetWindowAttrsA`; `WA_NotifyDepth` causes `WM_RaiseWindow`/`WM_LowerWindow`/`WM_MoveWindowInFrontOf`/`depth_window` to post an `IDCMP_NEWSIZE` message when the window's z-order changes; `WA_MenuHelp` and `WA_TabletMessages` set/clear the `IDCMP_MENUHELP` and `IDCMP_TABLET` flags respectively; `WA_PointerDelay` schedules pointer changes requested by `SetWindowPointerA` so the cursor updates after the specified delay instead of immediately. `WA_Colors`, `WA_Checkmark`, and `WA_AmigaKey` are stored and read back, but are not yet rendered as custom chrome/images. Moves/resizes the native WM window as needed. |
| `GetWindowAttrsA` | Implemented | Reads back the corresponding live window fields, flags, and stored tag values. |
| `SetScreenAttrsA` | Implemented | Full `SA_*` tag coverage: geometry, depth, detail/block pens, title, font, type, bitmap, display ID, colors, pens, behind/quiet/autoscroll/showtitle flags, full palette, color map entries, parent, draggable/exclusive/share-pens/interleaved/like-workbench/minimize-ISG, and other storage tags. `SA_DClip` and `SA_Overscan` are now enforced as positioning constraints: they define the allowed display rectangle (`SA_DClip` directly, `SA_Overscan` via `OSCAN_TEXT`/`OSCAN_STANDARD`/`OSCAN_MAX`/`OSCAN_VIDEO` percentages) and the screen's position/size is clamped to stay inside that rectangle. Changing either tag recomputes the screen geometry and redraws. `SA_Colors`, `SA_Colors32`, and `SA_Pens` are parsed into a screen-palette (16 or 32 entries depending on `SA_FullPalette` / `SA_ColorMapEntries`) and applied to the host `WB_*` globals so the desktop and window chrome render with the guest screen's colours. `SA_FullPalette` and `SA_ColorMapEntries` control the number of entries used and are reapplied when changed. `SA_Interleaved` sets/clears the `BMF_INTERLEAVED` bit in the screen's custom `BitMap` flags. `SA_ErrorCode` is honoured on `OpenScreen` failure: duplicate public-screen names write `OSERR_PUBNOTUNIQUE` (5) and allocation failures write `OSERR_NOMEM` (3). `SA_PubSig`/`SA_PubTask` are used to signal the owning task when a screen's public status changes. `SA_BitMap` and `SA_BackFill` changes also trigger a desktop redraw; the custom screen `BitMap` is rendered into the desktop backdrop. `SA_BackFill` is stored; if the value is a real guest `Hook` pointer it is invoked during desktop redraws with A0=Hook, A2=screen `RastPort` (a temporary one is allocated if the screen does not have a persistent `RastPort`), A1=screen `Rectangle`, otherwise values below 256 are still treated as a pen-index fallback. `SA_Draggable`, `SA_Exclusive`, `SA_SharePens`, `SA_LikeWorkbench`, and `SA_MinimizeISG` are stored and read back, but have no additional host-side behaviour yet. Refreshes the desktop title when needed. |
| `GetScreenAttrsA` | Implemented | Reads back the corresponding live screen fields, flags, and stored tag values. |
| `GetVisualInfoA` | Implemented | Allocates a real guest `VisualInfo` structure tied to the supplied screen, including a freshly allocated `DrawInfo`. |
| `FreeVisualInfo` | Implemented | Frees the `DrawInfo` and the `VisualInfo` structure. |

`WA_*` and `SA_*` tag values, `WFLG_*` window flags, and screen type flags now match the AmigaOS 3.x definitions. Tags that only need to be readable (e.g., `WA_BackFill`, `WA_Colors`, `SA_BackFill`, `SA_Draggable`, `SA_Exclusive`, `SA_SharePens`, `SA_LikeWorkbench`, `SA_MinimizeISG`) are stored in the internal `IntuitionSlot` / `ScreenSlot` so they can be read back with `GetWindowAttrsA` / `GetScreenAttrsA`. Previously storage-only tags that now have live behaviour include `WA_PointerDelay` (delayed pointer update), `WA_AutoAdjust` (window clamped to screen), `WA_NotifyDepth` (`IDCMP_NEWSIZE` on depth changes), `WA_MenuHelp` / `WA_TabletMessages` (`IDCMP_MENUHELP` / `IDCMP_TABLET` flags), `SA_ErrorCode` (failure-code written on `OpenScreen` failure), `SA_FullPalette` / `SA_ColorMapEntries` (palette entry count), and `SA_Interleaved` (`BMF_INTERLEAVED` bit in the custom `BitMap`). `WA_Zoom` is now enforced: the existing WM zoom gadget toggles the window between the two rectangles in the zoom array. `WA_HelpGroup` / `WA_HelpGroupWindow` are enforced: the F1 key posts an `IDCMP_HELP` message to the focused window or to the designated help-group window. `SA_Parent` is enforced when opening a screen by inheriting the parent screen's dimensions and default pens if the child did not supply them. `SA_DClip` and `SA_Overscan` now define an active constraint rectangle for each screen: `SA_DClip` uses the supplied `Rectangle` directly, while `SA_Overscan` maps to a centred rectangle (`OSCAN_TEXT` 90%, `OSCAN_STANDARD` 95%, `OSCAN_MAX` 98%, `OSCAN_VIDEO` 105%) of the framebuffer. When a screen is opened, `MoveScreen()` is called, or `ScreenPosition()` is invoked, the screen's `LeftEdge`/`TopEdge`/`Width`/`Height` are clamped to remain within this rectangle. `SA_PubSig` and `SA_PubTask` are already enforced as described above.

**Backing semantics:** `WA_SuperBitMap` is now wired end-to-end: the supplied guest `BitMap` pointer is stored in the `IntuitionSlot`, the window `RastPort` is pointed at that `BitMap`, and the WM draw callback renders the backing bitmap into the window client area before host-side gadgets are drawn. `SA_BitMap` is rendered by the desktop backdrop path: `Desktop_Draw()` calls `UAOS_Intuition_RenderScreenBackdrop()`, which uses the existing `render_bitmap_to_framebuffer()` helper to blit the screen's custom planar `BitMap` through its `ViewPort` `ColorMap` into the host framebuffer. `WA_BackFill` / `SA_BackFill` hooks are stored as pointers; if the stored value is a real guest `Hook` pointer it is now dispatched via `UAOS_InvokeM68kHook()` using the AmigaOS Hook convention (A0=Hook, A2=RastPort, A1=Rectangle). A small hook value (less than 256) is still treated as a pen index and the corresponding window client area or full desktop is filled with that pen's RGB colour.

**Palette semantics:** `SA_Colors` (classic `ColorSpec` array), `SA_Colors32` (32-bit `LoadRGB32`-style table), and `SA_Pens` (DrawInfo pen index array) are parsed and applied to the host `WB_*` palette globals. `Desktop_Draw()` applies the front screen's palette before rendering the desktop chrome, and the WM calls an intuition-supplied palette callback before drawing each window's chrome so the window adopts the colours of the screen it lives on. The default mapping uses screen colours 0-3 for `WB_GREY`/`WB_BLACK`/`WB_WHITE`/`WB_BLUE`; `SA_Pens` entries override these roles (`DRI_BACKGROUNDPEN`, `DRI_TEXTPEN`, `DRI_SHINEPEN`, `DRI_SHADOWPEN`, `DRI_FILLPEN`, `DRI_FILLTEXTPEN`). Screens without custom colours fall back to the default Workbench 3.x palette.

### IDCMP event injection

| Event class | Status | Notes |
|-------------|--------|-------|
| `IDCMP_MOUSEBUTTONS` | Implemented | Mouse button press/release on the window client area is converted to window-relative coordinates and posted to the `UserPort`. |
| `IDCMP_MOUSEMOVE` | Implemented | Mouse moves over the focused window are posted with window-relative coordinates. |
| `IDCMP_RAWKEY` / `IDCMP_VANILLAKEY` | Implemented | WM keystrokes are posted as raw/vanilla key events unless they are consumed by an active string gadget (character insertion, backspace, cursor movement, selection, etc.). |
| `IDCMP_CLOSEWINDOW` | Implemented | Clicking the window close gadget is vetoed at the WM level and posted as an `IDCMP_CLOSEWINDOW` message; the guest must call `CloseWindow()` to actually close it. |
| `IDCMP_GADGETDOWN` / `IDCMP_GADGETUP` | Implemented | The WM's existing system gadgets (close, drag, depth, size) are exposed as guest `Gadget` structures and linked into `Window.FirstGadget`. Pressing and releasing a system gadget posts the corresponding `IDCMP_GADGETDOWN`/`IDCMP_GADGETUP` message with the gadget pointer in `IAddress`. |
| `IDCMP_NEWSIZE` | Implemented | Posted when the window is resized via the resize grip or via `SizeWindow()`. |
| `IDCMP_ACTIVEWINDOW` / `IDCMP_INACTIVEWINDOW` | Implemented | Focus changes (raise/lower/close/click-to-focus) are posted to the `UserPort`. |
| `IDCMP_HELP` | Implemented | The F1 key is mapped to a help keystroke and posts an `IDCMP_HELP` message; if `WA_HelpGroupWindow` is set, the message is sent to that window instead of the focused window. |

| Function | Status | Notes |
|----------|--------|-------|
| `StripIntuiMessages` | Implemented | Walks the supplied `MsgPort` and removes every `IntuiMessage` whose class matches the supplied IDCMP mask, returning the number of removed messages. |

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
| `MoveScreen` | Implemented | Updates the guest `Screen` `LeftEdge`/`TopEdge` fields and clamps the new position to the screen's `SA_DClip` / `SA_Overscan` constraint rectangle, if any. |
| `ScreenToFront` | Implemented | Marks the screen as the active screen and refreshes the desktop title. |
| `ScreenToBack` | Implemented | Marks the screen as behind and picks another active screen for the title. |
| `ScreenDepth` | Implemented | V39 depth-arrangement: `SDEPTH_TOFRONT`/`SDEPTH_TOBACK` adjust the front/back flag. |
| `ScreenPosition` | Implemented | V39 screen positioning: `SPOS_RELATIVE` moves by `(x1,y1)`, `SPOS_ABSOLUTE` sets position, `SPOS_MAKEVISIBLE` clamps a rectangle into view. Final position is clamped to the `SA_DClip` / `SA_Overscan` constraint rectangle. |
| `ShowTitle` | Implemented | Toggles the `SHOWTITLE` flag and shows/hides the screen title in the desktop menu bar. |

Screens are mapped to the single UAOS desktop. Multiple screens are tracked in a small internal table; the frontmost screen with `SHOWTITLE` set controls the title displayed in the native desktop menu bar. `SA_DClip` and `SA_Overscan` define a constraint rectangle that controls the screen's initial geometry and limits subsequent moves via `MoveScreen()` and `ScreenPosition()`. `SA_Colors`, `SA_Colors32`, and `SA_Pens` are parsed into a host RGB palette and applied to the desktop and window chrome. If a screen is opened with `SA_BitMap`, or `SA_BitMap` is later changed via `SetScreenAttrsA()`, the custom planar `BitMap` is rendered into the desktop backdrop through the screen's `ViewPort` `ColorMap`.

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
| `SetWindowPointerA` | Implemented | Supports `WA_BusyPointer` (TRUE installs a busy/hourglass cursor, FALSE restores the default), `WA_Pointer` (renders the custom pointer), and `WA_PointerDelay` (milliseconds to delay the pointer change; the change is scheduled and applied by the cursor module once the delay elapses, so rapid pointer changes do not flicker). A tag-list with no tags clears the custom pointer. |

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
| `ViewAddress` | Implemented | Returns a pointer to a single guest `View` structure allocated in the Intuition heap and zeroed on first use. |
| `ViewPortAddress` | Implemented | Returns a pointer to the screen's `ViewPort` (uses `Window.WScreen`). |
| `GetScreenData` | Implemented | Copies up to 256 bytes from the appropriate screen: `CUSTOMSCREEN` (type 0) uses the supplied `Screen` pointer; `WBENCHSCREEN` uses the Workbench screen; `PUBLICSCREEN` uses the default public screen. Any other type value falls back to the supplied `Screen` pointer if it is non-zero and within guest RAM; otherwise the buffer is zeroed. |
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

The desktop menu bar now renders the active guest window's `MenuStrip`. When a guest window is focused, its `Menu` titles are parsed and drawn in the menu bar; opening a menu and selecting an enabled item posts an `IDCMP_MENUPICK` message with the classic packed menu number `(menu | item << 5 | sub << 11)`. Submenus are parsed from `MenuItem.SubItem` and drawn as cascading menus. Items with `CHECKIT` show a check box that is filled when checked; `MENUTOGGLE` items toggle their checked state on selection, while plain `CHECKIT` items are set. `COMMSEQ` command-key shortcuts are parsed, displayed next to the item label, and now invoked from the keyboard: pressing the command key (or Ctrl+letter as a Right-Amiga substitute) for an enabled item updates its check state and posts `IDCMP_MENUPICK` to the focused window. Disabled items (`ITEMENABLED` clear) are greyed out and cannot be selected. When no guest window has a menu strip, the desktop falls back to the built-in Workbench menu.

Mouse and repeat queues are enforced: `WA_MouseQueue` limits pending `IDCMP_MOUSEMOVE` messages, and `WA_RptQueue` limits pending `IDCMP_MOUSEBUTTONS` messages. When a new message would exceed the queue limit, the oldest queued message of that class is removed before the new one is added. A limit of 0 means no restriction.

## BOOPSI — built-in classes

The BOOPSI registry and dispatch code (`NewObjectA`, `DisposeObject`, `SetAttrsA`, `GetAttr`, `GetAttrsA`, `DoMethodA`, `DoSuperMethodA`, `MakeClass`, `AddClass`, `FreeClass`, `RemoveClass`) are implemented. A `CLASS_FLAG_NATIVE` flag lets a class store a host C dispatcher instead of an M68k hook entry, and the dispatcher supports `DoSuperMethodA` by walking the class superclass chain.

At boot, the following standard built-in classes are registered automatically:

| Class | Super class | Notes |
|-------|-------------|-------|
| `rootclass` | — | Base `OM_NEW`/`OM_DISPOSE`/`OM_SET`/`OM_GET`/`OM_ADDTAIL`/`OM_REMOVE` support. |
| `gadgetclass` | `rootclass` | Object is laid out as a real AmigaOS `Gadget` structure (44 bytes, offset 0) so it can be added directly to a `Window.FirstGadget` list. `GA_*` tags set `Gadget` fields, create an `IntuiText` label for `GA_Text`/`GA_Label`, and set `GFLG_LABELITEXT`. `GM_HITTEST` checks the gadget bounds, `GM_RENDER` draws a bevelled button with the label, `GM_GOACTIVE` selects the gadget (toggling for `GACT_TOGGLESELECT`), `GM_HANDLEINPUT` keeps the gadget active while the mouse is inside and returns `GMR_MEACTIVE`, and `GM_GOINACTIVE` clears the selected state. |
| `imageclass` | `rootclass` | Handles `IA_Width`, `IA_Height`, `IA_FGPen`, `IA_BGPen`, `IA_Data`, `IA_Left`, `IA_Top`, plus `IM_BitMap`. |
| `pointerclass` | `imageclass` | Handles `POINTERA_BitMap`, `POINTERA_XOffset`, `POINTERA_YOffset`, `POINTERA_WordWidth`, `POINTERA_XResolution`, `POINTERA_YResolution`, `POINTERA_Flags`. `WA_Pointer` now checks for a pointerclass object first and reads its BitMap/offsets directly instead of relying on the old heuristic scan. |
| `menuclass` | `rootclass` | Real menu node structure with `MA_Type`, `MA_Label`, `MA_Key`, `MA_Disabled`, `MA_Checked`, plus `MA_AddChild`/`MA_RemChild`. `OM_ADDMEMBER`/`OM_REMMEMBER` manage a linked list of child menu nodes, and `OM_DISPOSE` recursively disposes children and siblings. |
| `windowclass` | `rootclass` | Real window attribute storage: `WA_Left`, `WA_Top`, `WA_Width`, `WA_Height`, `WA_Title`, `WA_Flags`, `WA_IDCMP`, `WA_CustomScreen`/`WA_PubScreen`, and min/max bounds. `OM_GET`/`OM_SET` read and write these attributes. |

Built-in classes are created in `kernel/exec/boopsi_builtin.c` and registered from `UAOS_INTUITION_Register()`. The class structures are stored in the same public registry used by `MakeClass`/`AddClass`, so `NewObject(NULL, "gadgetclass", ...)` finds them by string ID.

## Guest data structures

The guest `Window` structure is kept AmigaOS 3.x-compatible at the offsets used by typical M68k binaries:

- `LeftEdge` / `TopEdge` / `Width` / `Height` at offsets 4, 6, 8, 10.
- `RPort` pointer at offset 50.
- `BorderLeft` / `BorderTop` / `BorderRight` / `BorderBottom` at offsets 54, 55, 56, 57.
- Standard `WFLG_*` window flags and `IDCMP_*` IDCMP flags are defined.

The guest `Screen` structure now includes `DetailPen`/`BlockPen` (offsets 70/71), `RastPort` pointer (offset 80), `Depth` (offset 84), `BitMap` (offset 88), `DisplayID` (offset 92), and `Colors` (offset 96). The `DrawInfo` structure is allocated with 16 drawing pens, the screen's font pointer, and default depth/resolution fields.

Intuition uses a dedicated 64 KiB guest heap (below the stack) for small allocations such as `Window`, `Screen`, `RastPort`, `MsgPort`, `IntuiMessage`, and `DrawInfo` structures. The heap is now a free-list allocator with block headers, so `CloseWindow`, `FreeSysRequest`, `CloseWorkBench`, and `FreeScreenDrawInfo` reclaim the memory they allocate.

The `IntuitionSlot` and `ScreenSlot` host structures cache tag values that have no direct live effect (e.g., `WA_Colors`, `WA_Checkmark`, `WA_AmigaKey`, `WA_MenuHelp`, `WA_TabletMessages`, `WA_AutoAdjust`, `WA_NotifyDepth`, `WA_PointerDelay`, `WA_Zoom`, `SA_ErrorCode`, `SA_FullPalette`, `SA_ColorMapEntries`, `SA_Interleaved`, `SA_SharePens`, `SA_Exclusive`, `SA_Draggable`, `SA_LikeWorkbench`, `SA_MinimizeISG`, `SA_BackFill`, `SA_Parent`) so that `GetWindowAttrsA` and `GetScreenAttrsA` can return the values supplied by the guest.

`GimmeZeroZero` windows store their border sizes in the `IntuitionSlot` and report content-relative mouse coordinates in `IDCMP` messages. The window `RastPort` stores the guest `Window*` pointer in its `Layer` field (`RP_OFF_LAYER`), and `graphics.library` adds the window's screen position plus the GimmeZeroZero border offset to all framebuffer drawing operations so that guest drawing commands are relative to the window content area.

## Window manager integration

The Intuition library keeps a small internal table (`IntuitionSlot`) that maps guest `Window` pointers to host WM handles. All ordering/repaint requests are forwarded to the host WM:

- `WM_RaiseWindow` — bring a window to front.
- `WM_LowerWindow` — send a window to back.
- `WM_MoveWindowInFrontOf` — insert a window at a specific z-order slot directly in front of another window.
- `WM_RequestFocus` — activate a window.
- `WM_RepaintWindow` — request a chrome/content repaint.

## LVO offsets

Intuition LVO offsets in `emulation/uaos_m68k_glue.c` match the AmigaOS 3.x `intuition_lib.fd` file for the classic functions. Recent additions and corrections include `OnMenu` (-114), `OffMenu` (-108), `ViewAddress` (-126), `ViewPortAddress` (-132), `GetScreenData` (-306), `LockIBase` (-294), `UnlockIBase` (-300), `LockPubScreen` (-384), `UnlockPubScreen` (-390), `LockPubScreenList` (-396), `UnlockPubScreenList` (-402), `NextPubScreen` (-408), `SetDefaultPubScreen` (-420), `PubScreenStatus` (-426), `GetDefaultPubScreen` (-432), `SysReqHandler` (-450), `GetVisualInfoA` (-630), and `FreeVisualInfo` (-636). Additional LVOs now installed: `MoveWindowInFrontOf` (-516), `SetEditHook` (-510), `ObtainGIRPort` (-456), `ReleaseGIRPort` (-492), `StripIntuiMessages` (-504), `NewObjectA` (-48), `DisposeObject` (-168), `SetAttrsA` (-180), `GetAttr` (-192), `AddClass` (-198), `GetAttrsA` (-204), `RemoveClass` (-210), `NextObject` (-216), `DoGadgetMethodA` (-222), `SetSuperAttrsA` (-228), `DoMethodA` (-258), `DoSuperMethodA` (-288), `CoerceMethodA` (-312), `MakeClass` (-330), and `FreeClass` (-336). These LVOs use the same function indices as the internal dispatch table in `kernel/exec/intuition_lib.c` (indices 97–116).

Newly wired LVOs in this session: `HelpControl` at the AmigaOS 3.x offset `-828`; `StartScreenNotifyTagList` (-900), `EndScreenNotify` (-906), `GetWindowAttr` (-912), `SetWindowAttr` (-918), `GetScreenAttr` (-924), `SetScreenAttr` (-930), `NewObject` (-936), `SetAttrs` (-942), `GetAttrs` (-948), `DoMethod` (-954), `DoSuperMethod` (-960), `CoerceMethod` (-966), `SetGadgetAttrsA` (-972), `SetSuperAttrs` (-978), `SetWindowPointer` (-984), `OpenWindowTags` (-990), `OpenScreenTags` (-996), `DoGadgetMethod` (-1044), and `SetGadgetAttrs` (-1050). The non-A varargs wrappers are not standard AmigaOS 3.x LVOs (they are normally provided by `amiga.lib`); they are assigned custom LVO slots in the unused range and are wired to the internal dispatch table indices 117–136. `SetGadgetAttrsA` shares the existing `SetAttrsA` dispatcher path; the new `SetGadgetAttrs` (non-A) wrapper builds a tag list from the guest stack and calls `SetGadgetAttrsA`.
