---
type: Kernel Subsystem
title: Display and Window Manager
description: The UAOS graphical environment, including the linear framebuffer and windowing system.
resource: /kernel/display/
tags: [display, wm, framebuffer, gui]
timestamp: 2026-10-02T00:00:00Z
---

# Display and Window Manager

UAOS provides a graphical user interface (GUI) inspired by the Amiga Workbench. It operates directly on a linear framebuffer.

## Linear Framebuffer

The framebuffer is initialized during boot via Multiboot2 tags. UAOS supports 32-bit color (BGRA/RGBA) and provides primitives for:
- Drawing pixels, lines, and rectangles.
- Rendering bitmaps and icons.
- Font rendering (8x16 and 8x8 bitmap fonts).

### Double Buffering and Dirty-Rect Tracking

All WM-driven rendering goes through a back buffer (`g_backbuf`, 1440×1024 max in BSS). The pipeline is:

1. `FB_BeginDraw()` — switches primitives to back-buffer mode and resets the dirty rectangle. BeginDraw/Flip nest (`g_draw_depth`): an inner pair merges into the outermost frame's dirty box and only the depth-0 Flip commits — a paint callback that re-enters the frame machinery can no longer reset the in-flight dirty rect or drop `g_drawing` mid-frame (UAOS-190).
2. Primitives (`FB_FillRect`, `FB_DrawHLine`, `FB_DrawVLine`, `FB_PutChar`, `FB_PutCharSmall`, `FB_BlitARGB`, `FB_BlitRow`, `FB_PutPixel`) paint into the back buffer and extend a bounding-box dirty rectangle (`g_dirty_x0/y0/x1/y1`).
3. `Cursor_Redraw()` draws the cursor sprite into the back buffer (its pixels are included in the dirty rect automatically).
4. `FB_Flip()` — `memcpy`s only the dirty rows from the back buffer to VRAM (32bpp uses row `memcpy`; 24bpp uses a per-pixel loop). If nothing changed, the flip is a no-op. A flip whose dirty box covers the whole screen sets `g_bb_coherent`.

**Direct-mode mirroring (UAOS-193):** every primitive also mirrors its writes into `g_backbuf` when not drawing, so the back buffer converges to screen contents even outside BeginDraw frames. Once one full-screen flip has run (`g_bb_coherent == 1`) the back buffer is authoritative for every pixel: `FB_GetPixel()` and the cursor's background save read ordinary cached RAM instead of VRAM — each uncached VRAM read is a bus transaction on the MBP4,1 G84 (which boots `nomtrr`). `FB_BackbufRow(y)` exposes a row for direct copies (cursor save/restore) and `FB_BackbufCoherent()` reports validity; before coherence, readers fall back to VRAM.

### Scene Clip Rectangle

`FB_SetClipRect(x, y, w, h)` / `FB_ClearClip()` install an optional half-open screen-space clip (`g_clip_*`) that every write primitive honours in addition to the framebuffer-bounds clamp: `FB_PutPixel` and the char paths test each pixel (`clip_point`), while the rect/line/blit fills intersect their bounds up front (`clip_rect`). `FB_BlitARGB` computes its source `skip` offset after clipping so the blit stays aligned, and `FB_FillRectDithered` clips after the fb-bounds clamp so `(px - x)`/`(py - y)` checkerboard parity stays anchored to the requested origin. The clip applies in both back-buffered and direct modes; it is off unless explicitly set, so full-scene repaints are unaffected. Its only consumer is `repaint_damaged()` (see below).

### Fast Row-Based Primitives

The hot primitives (`FB_FillRect`, `FB_DrawHLine`, `FB_DrawVLine`, `FB_PutChar`, `FB_PutCharSmall`) hoist the `g_drawing` and `bpp` branches out of the per-pixel loop, resolve the target row pointer once, and run a tight inner loop. Solid fills (`FB_FillRect`, `FB_DrawHLine`) use `rep stosl` dword stores in both the back-buffer and 32bpp VRAM paths (UAOS-193). `FB_BlitARGB` provides a clipped ARGB row blit (alpha-keyed, optional colour inversion) used by `icon_render.c` and the cursor sprite instead of per-pixel `FB_PutPixel` calls; `FB_BlitRow` is its opaque variant (no alpha test, straight copy). `FB_FillRectDithered` paints a 1px two-colour checkerboard rect (pattern anchored to the rect origin) with the same hoisted structure — used by the scrollbar track (UAOS-103). The freestanding `memcpy`/`memset` emitted by `scripts/build_iso.sh` (`build/obj/stubs.c`, do not edit directly) use `rep movsl`/`rep stosl` dword bodies plus byte tails — integer-only, no SSE (the scheduler does not context-switch XMM state); this alone removes most of the per-flip VRAM copy cost (UAOS-193).

### Mode-Size Clamp

`FB_Init` clamps `g_fb.width`/`g_fb.height` to the back-buffer dimensions (`BB_MAX_W` × `BB_MAX_H` = 1440×1024) so the desktop always lays out inside the drawable region even if GRUB selects a larger mode.

### String Clipping

`FB_PutStrCentred` and `FB_PutStrSmallCentred` truncate the string to fit the target rectangle (character-granular), preventing long window titles from overdrawing the zoom/depth gadgets or bleeding past the window edge.

The buffered (`g_drawing`) paths of `FB_PutChar`/`FB_PutCharSmall` clip rows the same way as the direct-to-VRAM path: `continue` for `py < 0` and `break` only once `py >= BB_MAX_H`, so a glyph whose top extends above the screen still draws its visible lower rows instead of vanishing entirely.

## Boot Splash

`kernel/display/splash.c` paints a boot splash onto the framebuffer during early kernel init (UAOS-151). The artwork lives at `assets/splash.jpg`; at build time `tools/make_splash.py` converts it to a self-describing RGB24 blob (`"SPL0"` magic, width, height, border colour, row-major pixels) which `ld -r -b binary` links into the kernel as `_binary_splash_rgb_start/_end` — no decoder needed in-kernel.

`Splash_Show()` fills the whole framebuffer with the blob's border colour (so letterbox bands blend) and centre-blits the image via `FB_BlitARGB` row-by-row through a `0xFF`-alpha conversion buffer (centre-cropped if the art exceeds the mode). It is called twice in `uaos_kernel_main`: once right after `UAOS_MMU_Init()` (required — the FB can sit above the bootstrap 1 GB identity map), and again after the chipset self-tests because they scribble into the same framebuffer. `Splash_Dwell()` then holds the second paint for ~1 s with a pre-PIT `pause` spin — init is fast enough that the splash would otherwise only flash by. GRUB covers the menu phase independently: `scripts/grub.cfg` loads `insmod jpeg` and sets `background_image /boot/splash.jpg`, staged onto the ISO by `build_iso.sh`.

## Screen Capture

`C:screenshot` ([screenshot.md](screenshot.md)) writes the composited screen to a baseline JPEG on `RAM:` via `jpeg_enc.c` (in-kernel encoder) — filename is a `YYYYMMDDHHMMSS` serial from NTP/RTC time.

## Window Manager (WM)

The Window Manager (`wm.c`) manages a z-ordered stack of windows. It handles user interaction and repainting.

### Key Features
- **Z-Order Management**: Windows are stacked, with the top window receiving focus.
- **Modal windows**: `WM_SetModal()` marks a window modal. While any active window has `modal` set, `hit_test` skips every non-modal window — the modal can never be buried under a raised window (which would deadlock a caller blocked waiting for its click). Cleared automatically when the slot is recycled by `WM_AddWindow`'s `memset`. Used by Intuition requesters, alerts, and the ASL file requester.
- **Click-to-Focus**: Clicking a window title bar or client area raises it to the top.
- **Raise / Lower**: `WM_RaiseWindow` brings a window to the front; `WM_LowerWindow` sends it to the back. `WM_MoveWindowInFrontOf` and the depth gadget also reorder the z-stack. Whenever the z-order of a window changes, `UAOS_Intuition_NotifyDepthChange()` is called so windows with `WA_NotifyDepth` can receive `IDCMP_NEWSIZE`.
- **Repaint Requests**: `WM_RepaintWindow` requests a chrome/content redraw of a window — damage-scoped (UAOS-101): only the window's footprint plus other windows intersecting it are repainted at the next flush.
- **Title Changes**: `WM_SetWindowTitle` updates the title string stored in the `WmWindow` and damages only the title-bar strip.
- **Slot reuse**: `g_wins[]` slots are recycled on close; `WM_AddWindow` `memset`s the whole `WmWindow` before initialising so a reused slot can't inherit the previous window's `view_h`, `zoomed`, restore geometry, or stale callbacks (UAOS-94).
- **Dragging & Resizing**: Title bars can be dragged to move windows, and some windows support resizing.
- **Event Routing**: Mouse and keyboard events are routed to the active window's callbacks.

### Damage-Scoped Repaint (UAOS-101)

`WM_Redraw()` previously repainted the entire scene (backdrop, every icon, every window, menu, cursor) on every event — scrollbar clicks, menu hover, drag/resize mouse moves, focus changes, and the 1 Hz clock tick. Event handlers now instead merge a damaged screen rectangle into `g_dmg_*` bounds (`damage_add`) and the event pump repaints the union once per iteration via `WM_FlushRedraw()` → `repaint_damaged()`:

1. `FB_BeginDraw()` — switches to the persistent back buffer (pixels outside the damage keep their previous contents).
2. `FB_SetClipRect()` constrains every write primitive to the damage rect for the rest of the scene pass. This is required for correctness (UAOS-121): `repaint_window()` paints each intersecting window's *full* footprint and `FB_Flip()` copies the union dirty box, so without the clip a lower window's repaint overwrites the back-buffer pixels of a front window that was skipped for not intersecting the damage — it appeared to pop to the front during title-bar drags until a later repaint restored it.
3. If the damage may expose desktop (`g_dmg_desktop`), `Desktop_RedrawRect()` repaints backdrop/icons/menubar inside the damage only — including the damaged region of the front Intuition screen's BitMap via `UAOS_Intuition_RenderScreenBackdropRegion()`.
4. Only windows intersecting the damage are repainted, back-to-front, so restacked/vacated regions resolve to the new z-order.
5. The open menu dropdown repaints itself (it floats above windows).
6. `FB_ClearClip()` then `Cursor_Redraw()` + `FB_Flip()` — the sprite must paint at its full live position, which may lie outside the damage; the VRAM copy is bounded by the FB dirty rect.

Menu footprint caveat (UAOS-122): `g_menu_*`/`g_submenu_*` are normally only refreshed inside `draw_menu_dropdown()` — i.e. at repaint time, *after* damage is collected. `menu_invalidate()`/`menu_invalidate_items()` therefore call `menu_update_geometry()` (which replicates the draw-time layout math) and damage the union of the last-drawn rect and the rect the current state will produce. Without this, opening or switching to a menu whose footprint differs from the previously drawn one clips the new dropdown to the stale damage rect — visible as items cut off mid-glyph at the panel edge.

Two invalidation APIs mark damage: `WM_InvalidateRect()` (only window content changed) and `WM_InvalidateDesktopRect()` (the rect may expose backdrop/icons/menubar — a window vacated it, or desktop content itself changed). Callers: window drag/resize/zoom/depth, scrollbar and gadget presses, focus changes, `WM_SetWindowTitle` (title strip only), `WM_CloseWindow` (vacated footprint + new focus title bar), desktop menu open/close/switch, lasso old/new outline, icon select/drag/drop footprints, menubar clock tick, `Desktop_SetScreenTitle`, all prefs-window gadget handlers (UAOS-192), and shell-window keystroke/line-edit repaints (`inst_invalidate_input`/`inst_invalidate_client`, UAOS-284 — direct-mode erase+redraw per keystroke visibly flashed on the MBP4,1's uncached VRAM). A burst of input coalesces into one repaint instead of one per event.

**Frame ownership / serialization (UAOS-190):** the damage merge in `damage_add()` and the bounds snapshot+clear at the top of `repaint_damaged()` run under `irq_save()` — cli blocks the PIT tick, so a preempted task can't tear a half-merged damage box. `WM_FlushRedraw()` wraps `repaint_damaged()` in `Forbid()` so no other task can enter `FB_BeginDraw`/`FB_Flip` while the pump owns `g_drawing`, the FB dirty rect and the clip. `WM_Redraw()` itself no longer paints on arbitrary contexts (UAOS-192): off the event pump it just damages the full screen and wakes the pump (`EventPump_IsCurrent()` decides); on the pump it still paints synchronously (`redraw_full()`), which callers like the Format window's "Formatting..." status rely on before blocking. Consequently every `FB_BeginDraw..FB_Flip` frame in the system runs on pump context only — IRQ paths and other tasks only ever enqueue damage.

Because the sprite is painted into the back buffer at frame end and the back buffer persists, `repaint_damaged` first damages `Cursor_GetSpriteRect()` (union of the sprite's front- and back-buffer footprints) so the scene repaint erases stale sprite pixels before `cursor_save_bg` samples the new position — prevents ghosting and save-buffer contamination.

### Window Callbacks
Each window provides callbacks for:
- `draw`: Redrawing the client area.
- `on_key`: Handling keystrokes.
- `on_click`: Handling mouse clicks in the client area.

### Close and Depth Gadgets

`WM_CloseWindow` repaints via the double-buffered damage path — the vacated footprint is damaged with the desktop flag plus the new focus window's title bar (no flicker, no full-scene repaint). The title bar is 20 pixels high and uses the full 8×16 font; its close, zoom, and depth cells are square and have explicit separating edges. The zoom and depth glyphs use compact 11×7 imagery centered with at least three pixels of horizontal padding. Close, zoom, and depth actions are armed on mouse-down, rendered with an inset bevel and shifted glyph, and committed only when the left button is released over the same gadget. Releasing elsewhere cancels the action. The zoom gadget toggles between the full usable screen and the window's saved original geometry, then emits a resize event so Intuition's guest `Window` geometry stays synchronized. The depth gadget (`depth_window`) reorders the z-stack and notifies Intuition of the focus change via `wm_notify_focus_change()` so `IDCMP_ACTIVEWINDOW`/`INACTIVEWINDOW` are sent. The square bottom-right sizing gadget uses the same pressed bevel while its drag updates window geometry and emits resize events.

### Vacate hook

`WM_SetVacateFn()` registers an optional `WM_VacateFn` callback invoked with a window's old screen rectangle just before that rectangle is vacated (title-bar drag, `WM_MoveWindow`, `WM_SetWindowGeometry`, zoom toggle, resize drag, and `WM_CloseWindow`). Intuition registers `intu_screen_vacate`, which erases the vacated rectangle in the parent screen's planar `BitMap` (pen 0) so the next backdrop render does not resurrect stale window pixels — needed now that window `RastPort`s draw into the screen `BitMap` rather than directly into the framebuffer.

**Guest-screen emit ownership (UAOS-265):** the planar `BitMap`/`ColorMap` a front screen emits through live in the *owning M68k task's* RAM window, so `scr_decode_pens`/`scr_build_lut`/`scr_emit_pens` must run with `g_ram` bound to that owner — never ambient `g_ram` (the shared window on native context, or a *reused* window after the owner dies: same addresses, foreign memory → alien pen indices and LUT colours painted mid-frame). `UAOS_Intuition_FlushScreenBitmap` binds `slot->owner->m68k_ram` around the emit, and every render/palette/poll/vacate entry point first retires orphaned slots via `screen_is_orphaned()`. Owner teardown lives in `UAOS_Intuition_CleanupTask()` (see `okf/kernel/exec/intuition_library.md`), hooked from `Task_Exit` before `Task_ReleaseM68kRam` and from `stub_RemTask`.

### Scrollbars

Every window gets an always-on right (vertical) and bottom (horizontal) scrollbar: an `WM_ARROW_LEN` (11px) arrow button at each end, a dithered track between them (drawn by `FB_FillRectDithered`, a hoisted two-colour checkerboard fill that replaces ~`live_w×len` `FB_PutPixel` calls per scrollbar per repaint — UAOS-103), and a hollow raised-bevel thumb sized proportionally to `view/content` (minimum 8px). The thumb top travels `track_len - thumb_len` pixels over the scroll range `[0, content - view]`. Thumb drags map pointer delta to scroll delta through **that same travel range** (`dm * max_s / travel`) so the thumb tracks the pointer 1:1 — the drag handler must replicate `draw_scrollbar`'s geometry exactly (track = rect − `WM_ARROW_LEN`×2, same thumb clamp). `view` is `WmWindow.view_h` when set via `WM_SetScrollInfoEx` (shell/ed/vim reserve a status bar), else the client height; `draw_chrome`, `scroll_by`, `WM_SetScrollY`, and the drag handler all use it consistently so the drawn thumb position, the scroll clamp, and the drag inverse all agree. Both arrow buttons are skipped when the scrollbar's long axis can't fit them (`th`/`tw < WM_ARROW_LEN*2`), so degenerate rects never paint stray arrows over chrome.

**Paint-order caveat (UAOS-285):** `repaint_window` must paint chrome before the client (the body fill would erase chrome), but clients only learn a new geometry inside `w->draw` and push `WM_SetScrollInfoEx` there — after the scrollbars were already drawn with the previous geometry's metrics. The scrollbar well paint is therefore extracted into `draw_scrollbars()`: `repaint_window` snapshots the scroll metrics around `w->draw` and repaints both wells if any changed, so a zoom/resize frame ends with a correct thumb in the same flip. `WM_SetScrollInfo`/`WM_SetScrollInfoEx` also re-clamp `scroll_x`/`scroll_y` (`clamp_scroll`) — a grown view or shrunk content used to strand the offset beyond the new range (thumb visually clamped but clients read the stale offset back via `WM_GetScrollY`). `redraw_full` delegates to `repaint_window`.

## Software Cursor

The software cursor (`cursor.c`) uses save/restore of background pixels for flicker-free movement. Key rules:

- **IRQ-time moves** (UAOS-104): `Cursor_Move` is called from `PS2Mouse_IRQHandler` but only records the target position and sets `cur_moved` — the save/restore/draw passes no longer run at IRQ time. The pump applies a pending move once per iteration via `Cursor_Flush()` (restore old background, save new, draw — all on the visible buffer in direct mode), and `Cursor_Redraw()` at the end of a back-buffered repaint paints at the live position. Packet bursts coalesce to a single paint per frame; `UAOS_Intuition_CheckPendingPointer()` now runs from `Cursor_Flush`/`Cursor_Redraw` instead of IRQ context. `Cursor_GetSpriteRect()` exposes the sprite's footprint in either buffer so damage repaints can erase it (see Damage-Scoped Repaint).
- **Atomic position snapshot** (UAOS-189): `cursor_commit_draw()` latches `cur_x`/`cur_y` once under `irq_save()` and clears `cur_moved` in the same section. Previously it re-read the shared position for save, draw and the `drw_x/y` record separately — an IRQ-side `Cursor_Move` landing mid-commit produced a torn commit (background saved at A, sprite painted at B) whose later restore stamped stale pixels at the wrong spot — the stray-fragment glitch seen on the MBP4,1. `cur_x`/`cur_y` are `volatile`; a move arriving during the paint stays pending for the next flush.
- **Back-buffer save + mirrored restore**: `cursor_save_bg` copies rows out of `g_backbuf` via `FB_BackbufRow()` whenever the back buffer is authoritative (in-flight frame or `FB_BackbufCoherent()` — always true once the WM has flipped the full screen), falling back to a 32bpp VRAM row `memcpy` and then per-pixel `FB_GetPixel`. `cursor_restore_bg` writes rows back to VRAM *and* mirrors them into the back buffer so the shadow stays correct; the 24bpp path uses `FB_PutPixel`, which mirrors itself.
- **Row-blit sprite draw**: `cursor_draw` assembles one ARGB scanline per sprite row (`0xFF` alpha for opaque, `0` for transparent; double-pixel mode widens runs in place) and calls `FB_BlitARGB` once per row — ~16 blits for the stock pointer instead of ~230 function-called `FB_PutPixel`s.
- **Background save/restore**: `cursor_save_bg` reads via `FB_GetPixel` when neither fast path applies. `cursor_restore_bg` is a no-op during back-buffered drawing since the repainted region covers the sprite footprint (damage repaints always include `Cursor_GetSpriteRect()`).
- **Default colours**: `CURSOR_DEFAULT_BODY` is `0xFF2200` — the classic Workbench 3.x red arrow — with a black outline/shadow (`cursor.h`; UAOS-4). Pointer Prefs has no colour constants of its own; it reads and applies `Cursor_GetSettings()`/`Cursor_SetColors()`.

## Workbench Elements
- **Palette**: `WB_InitPalette()` installs the canonical Workbench 3.1 four-pen palette — grey `#AAAAAA`, black, white, and highlight blue `#3B67A2` (R:59 G:103 B:162) — into the runtime `WB_*` globals. `WB_LIGHT_GREY`/`WB_DARK_GREY`/`WB_LIGHT_BLUE` (`scale_rgb(blue, 3, 2)` = `#589AF3`), `WB_ORANGE`, and `WB_CREAM` are UAOS extension accents outside the four hardware pens. Intuition screens (`SA_Colors`/`SA_Colors32`/`SA_Pens`) and the Palette prefs editor can override them at runtime; `apply_prefs()` maps the 12-bit intuition colour registers (default colour3 `0x36A`, nearest nibble encoding of `#3B67A2`). Selected-state chrome uses complement mode (`colour ^ 0xFFFFFF`), so a selected blue label reads orange-tan `#C4985D` — the same result real Workbench complement rendering produces.
- **Backdrop**: Solid Amiga grey (`WB_GREY`, R:170 G:170 B:170). Can be toggled via Workbench ▸ Backdrop to hide/show desktop icons.
- **Menu Bar**: Fixed at the top of the screen. Right-click opens menus; a left press in the band that misses every menu title, the clock, and the screen-depth gadget is consumed — LMB on the screen bar does nothing (no lasso, no backdrop double-click). The rightmost cell is the screen depth gadget (overlapping-rectangles glyph): a click cycles to the next Intuition screen via `UAOS_Intuition_CycleScreen(1)` and shift-click steps backwards, mirroring the real screen-bar depth gadget.
- **Icons**: Desktop icons representing disks, tools, per-volume Trashcans, and leave-out shortcuts (placed by Icons ▸ Leave Out). Leave-out icons show a small shortcut arrow and open/run their target on double-click.
- **Trashcan** (UAOS-36): Not a hardcoded desktop icon — on Amiga the Trashcan is a real drawer (`VOL:Trashcan`) plus a `WB_GARBAGE`-typed `Trashcan.info`, created on the volume at format time (`VFS_CreateTrashcan`, called by every format path unless NOICON). `get_icons()` emits a Trashcan icon (bottom-right, stacking upward) for each mounted volume whose Trashcan drawer exists; `Icon_Load` supplies the `.info` image and a saved `do_CurrentX/Y` position when nonzero, falling back to the procedural `draw_trashcan_icon` when there is no `.info`. RAM: gets a Trashcan at `VFS_Init` (it is "formatted" at boot). Double-clicking a Trashcan icon opens `FileBrowser_Open("VOL:Trashcan")`.

### Icon Cache

The desktop icon list (including `.info` file loading and planar decoding) is cached in `get_icons()` and only rebuilt when the VFS mount table, AppIcon set, leave-out registry, or Trashcan set changes (mount count, any mount name, AppIcon count, `g_leaveout_version`, or the per-mount `VFS_IsDir("VOL:Trashcan")` bitmask differs from the cached fingerprint). This avoids reloading every `.info` from VFS on every frame and mouse event. Click/selection state persists in the cached `icons[]` array across calls.

On rebuild, every emitted `icons[]` slot is fully reset (`memset`) before repopulation and unused slots are zeroed, so a slot recycled from a previous icon type can't retain stale `is_trashcan`/`is_appicon`/`is_leaveout`/`leaveout_path`/`appicon_id` state. Leave-out label/path backing storage is indexed by the leave-out registry index (`li < MAX_LEAVEOUT`), not the running icon index (which can exceed the arrays' size). The mount-name fingerprint is bounded by `MAX_ICONS` on both store and compare.

### Clock and Memory Display

The menubar shows a clock (`HH:MM:SS` in white on blue) on the far right. When the NTP epoch is live it converts UTC→local via `ntp_get_epoch()` + `tz_offset_min()` (same as `C:date` and the Clock window); otherwise it reads `RTC_ReadTime()` directly (UTC). Fixed in UAOS-91 — it previously always showed raw RTC/UTC, diverging from local time once ntpd synced. Just to the left of the clock is a free-memory readout (e.g. `512K Free` in cream on blue), computed from `Mem_GetInfo()` (x64 heap free + M68k guest RAM free slots), and left of that a `CPU nn%` busy readout from `CpuFreq_BusyPercent()` (UAOS-272 — rides the same 1 Hz refresh; sits outside `menubar_clock_hit()`'s right-edge zone so clicks there never open the Clock window). All three strings are tick-cached (UAOS-105): `draw_menubar` regenerates them only when `Desktop_GetTick()` changes (1 Hz), so RTC port I/O / NTP conversion and `Mem_GetInfo` no longer run dozens of times per second during drags. `Desktop_UpdateClock` (called once per second from IRQ context) increments the double-click tick counter and sets a dirty flag; `Desktop_FlushClockRedraw` checks the flag and damages only the menubar strip (`WM_InvalidateDesktopRect(0,0,W,MENUBAR_H)`) — the clock no longer triggers a full-scene repaint every second (UAOS-101). Double-clicking the clock text opens the Clock window (`ClockWin_Open()`): `menubar_clock_hit()` replicates the draw layout to hit-test the clock area, and the press/release pair uses the same `g_tick`/`DBLCLICK_TICKS` double-click timing as icons and the desktop backdrop.

### Menu Bar

The menu bar displays the Workbench 3.1-style menu titles (`Workbench`, `Window`, `Icons`, `Tools`) plus a free-memory display on the right. The `Shell` and `UAOS` menus have been removed for OS 3.1 fidelity. The menus follow the classic Amiga press-and-drag behaviour:

1. **Press and hold** the right mouse button on a menu title to open its drop-down.
2. **Drag** the cursor over the items to highlight them; the highlight updates as the cursor moves.
3. **Release** the right mouse button on an item to trigger its action and close the menu.
4. Dragging the cursor over another menu title while the right button is held switches to that menu.
5. A left mouse click dismisses an open menu without triggering an action.

#### Workbench Drop-down Menu

The `Workbench` menu contains the following items:

| Item | Action |
|------|--------|
| Backdrop | Toggles desktop icon visibility (hides/shows icons; backdrop + menu bar remain). |
| Execute Command | Opens the Shell window. |
| Redraw All | Requests a full desktop/window repaint. |
| Update All | Refreshes all open browser windows. |
| Last Message | Replays the title and body of the last requester shown (confirm/string/info). |
| About | Opens the About window. |
| Quit | Shows a confirm requester; on confirmation, warm-reboots the system via the keyboard controller. |

#### Window Drop-down Menu

The `Window` menu contains the following items:

| Item | Action |
|------|--------|
| New Drawer | Prompts for a name and creates a new directory in the focused browser. The target browser path + handle are stashed in `g_pending_mkdir` when the requester opens, so the callback works regardless of focus. |
| Open Drawer | Opens the selected directory in a new browser window. |
| Close | Closes the focused browser window. |
| Update | Refreshes the focused browser's entries. |
| Select Contents | Selects all entries in the focused browser. |
| Clean Up | Clears drag offsets and redraws the focused browser. |
| Snapshot | Saves the focused browser's window position/size (in-memory); restored on reopen. |
| Show | Shows information about the selected icon (same as Icons ▸ Information). |
| View By ▸ | **Flyout submenu** with checkmark items: Icon (grid), Name (list, alphabetical), Date (list, newest first), Size (list, largest first), Type (list, by type then name). Switches the focused browser's view mode and re-sorts entries. |

#### Icons Drop-down Menu

The `Icons` menu (opened with a right-click on the `Icons` title) contains the following items. A horizontal divider separates the upper icon-management items from the lower destructive/utility items.

| Item | Action |
|------|--------|
| Copy | Copies the selected file to `RAM:`. |
| Rename | Prompts for a new name and renames the selected entry. |
| Information | Shows an information requester with name, type, size, protection flags, and path. |
| Snapshot | Saves the selected icon's current position in the drawer (in-memory). |
| Unsnapshot | Clears the saved position of the selected icon. |
| Leave Out | Places a desktop shortcut icon pointing to the selected file/drawer. Double-clicking opens (dir) or runs (file) the target. |
| Put Away | Removes the currently-selected leave-out desktop shortcut icon. |
| *divider* | Horizontal separator. |
| Delete | Confirms and moves the selected file to its own volume's `VOL:Trashcan/` drawer (permanent delete when the volume has no Trashcan — e.g. formatted NOICON). |
| Format | Opens the Format window for device selection and FAT32 formatting. |
| Empty Trash | Confirms and recursively deletes the contents of every mounted volume's `VOL:Trashcan` drawer (`VFS_ReadDir`-based, so handler-backed FAT32 volumes are covered; 6-deep nesting cap). |

#### Tools Drop-down Menu

The `Tools` menu contains the following items:

| Item | Action |
|------|--------|
| Exchange | Opens the Commodities Exchange window for managing commodity brokers. |
| Blanker | Cycles the screen blanker commodity state (active → sleeping → disabled). |
| *divider* | Horizontal separator. |
| Reset WB | Closes all browser windows and redraws the desktop. |

The active guest menu strip is cached (UAOS-105): `refresh_active_menus()` reparses the focused window's strip via M68k memory reads only when the (strip pointer, focus handle, desktop tick) key changes, instead of on every repaint. The 1 Hz tick leg means checkmark/enabled mutations under a stable strip pointer are still picked up once per second, and `Desktop_MouseEvent` force-refreshes the cache on menu open (`refresh_active_menus_forced`) so a freshly opened menu never shows stale item state. The menus are rendered by `desktop.c` and managed through a small internal state (`g_menu_index`, `g_menu_hover`, etc.). Menu items support a divider flag (`is_divider`) for separator lines, a checkmark column (`has_checkmark`/`checked` for toggle items like View By modes), and a flyout submenu pointer (`has_submenu`/`submenu`). The fallback desktop menu now supports flyout submenus (e.g., View By) with the same press-and-drag behaviour as the guest Intuition menu strip: hover tracking opens the flyout, `submenu_hit` resolves the hovered flyout item, and `Desktop_RightButtonRelease` dispatches the selected flyout action. The window manager tracks both left and right mouse buttons and forwards desktop events and hover tracking to highlight items and dispatch the selected action.

### Requesters

`requester.c` implements modal confirm/string/info requesters as real WM windows (`WM_AddWindow` + `WM_RaiseWindow`), so a requester holds WM focus while open. `g_req.wm_handle` is initialized to -1 at declaration — the "already active" guard in every `Requester_*` open relies on that sentinel, and a BSS-zeroed 0 would block the first open forever. The guard also verifies `WM_GetDrawFn(handle) == req_draw` so a stale handle (closed or reused slot) can't wedge the requester. Close-gadget clicks are routed through `req_event` (registered via `WM_SetEventHandler`), which vetoes the WM's own close and runs `Requester_Close` + a Cancel callback — otherwise `WM_CloseWindow` would bypass `Requester_Close` and leave `wm_handle` stale. Completion callbacks are always invoked **after** `Requester_Close()` — in `req_release` (mouse) and in `req_key` (Enter/Escape) — so focus has already returned to the window that opened the requester when the callback runs. Callbacks that need a target captured before the requester opened should stash it at dispatch time (e.g. `g_pending_delete`, `g_pending_mkdir` in `desktop.c`) rather than re-querying `WM_GetFocus`.

### Icon Selection State
Desktop icons can be selected with a single click or by lasso (rubber-band) drag. The selected icon is rendered using one of two methods:

1. **Selected planar image**: If the `.info` file contains a dedicated selected-state image, that image is drawn directly.
2. **Inverse/video fallback**: If no selected image is present, the normal icon is drawn with inverted RGB colours (preserving transparency).

The `IconImage` structure tracks whether a selected planar image was actually loaded (`has_selected`). The renderer (`icon_render.c`) chooses the appropriate method, and the desktop (`desktop.c`) tracks which icons are currently selected and requests a redraw when the selection changes.

### Lasso (Rubber-band) Selection
Dragging the left mouse button on the empty desktop backdrop activates lasso selection — a dashed black rectangle follows the cursor (the "marquee"). Any icon whose bounding box (`ICON_W` x `ICON_H`) intersects the lasso rectangle is selected; icons outside the rectangle are deselected. The selection updates dynamically as the drag proceeds. On button release, the lasso rectangle disappears but the selection is retained.

A lasso drag (where the cursor moved) is not counted as a desktop click, so it does not contribute to the double-click-to-open-Shell counter. Only a click on empty desktop without dragging counts toward the double-click.

This matches classic Workbench 3.x behaviour. The lasso state is tracked in `desktop.c` (`g_lasso_active`, `g_lasso_start_x/y`, `g_lasso_cur_x/y`, `g_lasso_moved`) and the dashed border is drawn by `draw_lasso()` after icons but before the menu dropdown and bars, clipped to the desktop backdrop area (below the menu bar).

### File Browser Listing and Refresh
Browser windows (`filebrowser.c`) enumerate their directory through `VFS_ReadDir`, which works for both RAMFS and handler-backed (FAT32) volumes — so DH0: windows list real disk contents, not just RAMFS volumes. Each browser holds a private snapshot buffer (`entry_buffer`, up to 32 entries, 31-char names). Because the snapshot is loaded once, browsers watch the VFS change counter: `browser_draw_impl` compares `VFS_ChangeSeq()` against the browser's `seen_seq` and calls `browser_reload()` when they differ, clearing selection/drag/lasso state since entry indices are stale after re-sorting. This makes directories created by shell commands (`makedir`) or guest `dos.library` calls appear automatically on the next redraw — Window ▸ Update (`FileBrowser_Refresh`) remains as a manual reload.

### File Browser Lasso Selection
Lasso selection is also available inside drawer windows (`filebrowser.c`). Dragging the left mouse button on empty space within a browser's icon grid area (below the path bar) activates a lasso rectangle. Any icon cell that intersects the lasso is selected. The browser uses a per-icon `selected[]` array for multi-selection, replacing the previous single-`selected_icon` model. Single-clicking an icon selects only that icon and cancels any active lasso. The lasso rectangle is clipped to the browser's client area below the path bar.

### Icon Drag and Drop (Drag-to-Copy / Drag-to-Trash)
Dragging a desktop icon (volume or leave-out shortcut) onto another volume icon copies the source's contents into the destination volume. This mirrors the classic Workbench behaviour of dragging a disk/drawer icon onto another disk to copy files. Dropping an icon onto the Trashcan deletes it instead.

- **Source**: Any volume icon (e.g. `RAM:`, `Workbench:`) or leave-out shortcut icon (file or directory). The Trashcan and AppIcons are not valid drag sources.
- **Target**: Any volume icon that is not the source (drag-to-copy), or the Trashcan (drag-to-delete). AppIcons are not valid drop targets. The Trashcan only highlights when the dragged icon is a valid source (volume or leave-out).
- **Copy operation**: On release onto a volume, `desktop_do_copy()` recursively copies the source path into `dst_vol/name` using `desktop_copy_dir()` for directories (which walks `RamFsNode->first_child` / `next_sibling`) and `desktop_copy_file()` for files (VFS open/read/write loop with a 512-byte buffer).
- **Trash operation**: On release onto the Trashcan, `desktop_confirm_delete()` opens the same "Delete 'name'?" confirm requester as Icons ▸ Delete; on confirm, `desktop_move_to_trash()` moves the source path into the Trashcan of its *own* volume (`VOL:Trashcan/name` — the volume prefix is extracted from the source path; `VFS_Rename`, with copy+delete fallback). A volume without a Trashcan drawer gets a permanent `VFS_Delete`. This is the same code path as `menu_action_icon_delete` — the pending target is stashed in `g_pending_delete` so both entry points share `delete_cb`.
- **Snap-back**: The dragged icon snaps back to its original position in both cases — drag-to-copy and drag-to-trash do not move the icon itself.
- **Visual feedback**: While dragging, the icon under the cursor that would be the drop target is highlighted with a 2px white outline (with a 1px dark border). The highlight is drawn by `draw_drop_target_highlight()` in both `Desktop_Draw` and the dirty-rect repaint path. The drop target index (`g_drop_target_idx`) is updated in `Desktop_MouseMove` via `icon_at_pos()`.

## Shared Gadget Set (gadgets)

`gadgets.{c,h}` (UAOS-124) is the single gadget vocabulary for all kernel-native tools. One `Gad` descriptor covers every control kind — button, checkbox, radio (mutual exclusion is owner-managed), cycle (click arrow-column lower half = previous, else next; `val` owns the index), slider (`GADF_VERTICAL`, `gad_slider_from_mouse`), string/integer fields (caller-supplied buffer + `focused` gate), label, group box, listview — plus chrome helpers `gad_bevel`/`gad_label`/`gad_gbox`/`gad_bg`/`gad_btn_row` (standard Apply/Save/Close row). Behaviour: `gad_draw` renders, `gad_hit` hit-tests (checkbox/radio include label width), `gad_event` consumes `GAD_DOWN/MOVE/UP/KEY` and returns `GADE_CLICK/CHANGE/NONE`. All controls render in WB 3.1 metrics on the 8×16 grid. `prefs_win.c`, `requester.c`, `format_win.c`, `exchange_win.c`, and `pointer_prefs.c` are all migrated — no private `Pw*`/`Btn` structs remain. See [gadgets](/kernel/display/gadgets.md).

## Kernel UI Backend (uibind)

`uibind.{c,h}` (UAOS-125) binds a `UINode` tree to a `WmWindow`: it registers draw/click/move/release/key/event callbacks, re-runs `ui_arrange` into the client rect every draw (resize/zoom reflow is automatic), renders leaf gadgets through the shared `Gad` set, and delivers `UIEV_*` events (click/change/key/close/resize) to one app callback. Gadget state lives in a per-bind `Gad` array (`UIBIND_MAX_GADS`=32, string bufs 64 chars each); the tree itself stays declarative. `UI_PAGE` gets a bevelled tab strip with active-child visibility; `UI_CUSTOM` nodes get their draw callback. Radio groups self-clear siblings; sliders capture drags; strings take keyboard focus. A fixed pool of 4 binds lives in BSS. `ui_prefs_row()` builds the standard centred Save/Use/Cancel row. `pointer_prefs.c` is converted end-to-end as the proof of concept — its whole window is one `ui_window(...)` expression. See [uibind](/kernel/display/uibind.md).

## .gui Interface File Format (uiformat)

`uiformat.{c,h}` (UAOS-127) parses a text `.gui` document into the identical `UINode` tree the C API builds — same dependency-free model (arena nodes, in-place strings, no libc), so it compiles for kernel, libuaos and the host test. Grammar: `window "Title" { vgroup { label "x" ; button "OK" id=save ; } }` with `id=`/`weight=`/`min=`/`max=`/`pad=`/`spacing=`/`group=`/`tab=`/`active=`/`sel=`/`value=`/`maxchars=`/`text=` attrs and `disabled readonly toggle selected center vertical` flags; `id=name` hashes via `ui_sym()`. Loading convention: `ENV:GUI/<tool>.gui` overrides `SYS:Prefs/GUI/<tool>.gui`, falling back to the compiled tree — `uibind_load_gui()` (kernel) and `uaos_ui_load()` (userspace). See [uiformat](/kernel/display/uiformat.md).

## Declarative UI Tree (uitree)

`uitree.{c,h}` is the dependency-free interface description + auto-layout engine (UAOS-130, umbrella UAOS-123): a window's UI is a tree of `UINode`s — `UI_VGROUP`/`UI_HGROUP`/`UI_PAGE` containers plus gadget leaves — laid out by `ui_measure()` (bottom-up natural sizes) and `ui_arrange()` (top-down rect distribution by `weight`). `UI_SPACER` absorbs free space; `UI_F_CENTER` keeps a node at natural size centred in its cell; `UI_PAGE` provides a tabbed container with `ui_page_tab_rect` for backend tab hit-testing. Nodes are allocated from a caller-supplied `UIArena` (no malloc needed) via `ui_*()` constructors; `ui_find`/`ui_node_at` give id lookup and hit-testing. The file compiles unchanged for the kernel, libuaos, and host tools — `tools/ui_layout_test.c` unit-tests it and gates the ISO build. See [uitree](/kernel/display/uitree.md).

## Application Windows

The display layer includes several Workbench-style application windows in addition to the file browser and shell:

- **About window (`about_win.c`)**: Shows UAOS version, build date, display resolution, and memory size.
- **Calculator (`calc_win.c`)**: Amiga-style four-function calculator with double-precision arithmetic.
- **Clock (`clock_win.c`)**: Digital time and date display, updated once per second from the RTC. Opened by double-clicking the menubar clock or via the `clock` shell command.
- **Network Info (`netinfo_win.c`)**: Displays interface IP, MAC, gateway, DNS, and DHCP status.
- **Vim Editor (`vim_win.c`)**: Modal text editor with Normal/Insert/Visual/Command modes, search, undo, and `S:vim.conf` configuration.
- **ED Editor (`ed_win.c`)**: AmigaED-style line editor with edit mode (type text, cursor movement) and command mode (ESC for commands: `w` save, `q` quit, `wq` save+quit, `/pat` search, `N` goto line). Supports both standalone WM windows and inline shell integration. Simpler than Vim — no modal confusion.
- **AmigaGuide Viewer (`system/userspace/guide.c`)**: Userspace x86-64 binary that parses and renders `.guide` files. Supports `@NODE`/`@ENDNODE`/`@PREV`/`@NEXT`/`@TITLE` directives, `@{"label" LINK target}` navigation links, `@{"label" SYSTEM command}` sensitivity links, bold/italic text attributes, word wrap, scrolling, and keyboard navigation. Launched via `C:guide` or `Tools:Guide`.
- **Early Startup Control (`early_startup.c`)**: Boot-time menu that appears during a 2-second countdown after kernel initialization. If a key is pressed, presents options: Normal Boot, Boot without Startup-Sequence, Boot to Shell only (no Workbench), and Display System Information. Uses the framebuffer directly (pre-scheduler, pre-WM). Sets `ShellWin_SetShellOnlyMode()` to prevent `LoadWB` in shell-only mode.
- **Pointer Preferences (`pointer_prefs.c`)**: Cursor size, double-pixel mode, and acceleration settings. Declarative UI (UAOS-125): the entire window is a `UINode` tree bound via `uibind` — labelled weight-1 button groups with `pressed` selection markers, centred Apply/Close row, ESC/close-gadget via `UIEV_KEY`/`UIEV_CLOSE`. Auto-sized and reflows on resize.
- **Preferences Suite (`prefs_win.c`)**: GUI editors for all AmigaOS 3.x Prefs programs — Palette, Time, IControl, Input, ScreenMode, WBPattern, Font, Serial, Printer, Locale. Each opens a WM window with AmigaOS-style gadgets (buttons, cycle gadgets, sliders, checkboxes). Palette editor persists to `ENVARC:Sys/palette.prefs` via IFF PREF format. Time editor reads/writes the RTC via `RTC_ReadDateTime()`/`RTC_SetDateTime()`.
- **Commodities Framework (`commodities.h/c`)**: Broker registry for commodities — background tools that can be controlled from Exchange. Supports up to 16 brokers with Active/Sleeping/Disabled states and enable/disable/sleep/wake callbacks.
- **Exchange Window (`exchange_win.c`)**: GUI window listing all registered commodity brokers with status indicators and Enable/Disable/Sleep/Wake/Cycle controls.
- **Screen Blanker (`blanker.h/c`)**: A commodity that blanks the screen after configurable inactivity timeout (default 60 seconds). Registers with the Commodities framework. `Blanker_Tick()` called from `Desktop_UpdateClock()` once per second; `Blanker_OnInput()` called from the event loop on any mouse/keyboard activity. `Blanker_Tick` runs inside the RTC IRQ handler and must not touch the framebuffer — it only latches `g_blanker_pending` (UAOS-191); the event pump performs the actual `FB_BeginDraw`/black-fill/`FB_Flip` from task context in `Blanker_Flush()` once per iteration (the RTC tick wakes the pump every second anyway). While `Blanker_IsBlanked()`, `WM_Redraw()` is a no-op and `WM_FlushRedraw()` drops queued damage so the 1 Hz clock flush (or any other repaint request) can't undo the blank; un-blank paths clear the flag before redrawing.
- **Format Window (`format_win.c`)**: AmigaOS-style Format window opened from Icons ▸ Format. Lists formattable block devices in a cycle gadget, a volume name text field, a **Trashcan checkbox** (the GUI equivalent of Format NOICON — unchecked formats without a `VOL:Trashcan`), and a Format button that confirms via requester then invokes `FAT32_Format()`, auto-mounts the result via `VFS_MountPartition()`, and calls `VFS_CreateTrashcan()` on the new volume.
- **Userspace GUI Window (`user_window.c`)**: Backing for native Ring-0 userspace programs that use the GUI syscall interface.

## Shell Window

`shell_win.c` implements the graphical CLI window. It provides scrollable history, input line editing, output buffering, and synchronous child tracking. It can host multiple independent shell instances and dispatches commands to the native command table, resident commands, external ELF64 userspace programs, or embedded M68k binaries.

### Remote Shell Sessions

The same `ShellInstance` machinery also backs headless remote shells for
`C:telnetd`.  Slots `MAX_SHELLS..TOTAL_SHELLS-1` (4 remote shells) are
reserved so the window-shell allocator and the `WM_IsWindowActive`
reclaim path never see them; a remote instance has `wm_handle == -1`,
`remote == 1`, and owns a TCP socket index.

- `ShellWin_RemoteOpen(sock, gen)` initialises a slot, sends the banner
  and prompt, and spawns the usual `shell_task_entry` task — so command
  dispatch, aliases, env vars, pipes, background jobs and `NativeCmdCtx`
  callbacks behave identically to a window shell.  The `gen` argument is
  the TCP connection generation from `tcp_accept()` (UAOS-263), stored
  in the instance as `remote_gen`; if `Task_CreateNative()` fails the
  remote slot is rolled back (previously `remote_inuse` stayed set
  forever).  It returns an opaque
  handle: the slot index in the low 16 bits plus a per-open token in the
  high bits (`remote_token`, never 0).  `RemoteIsDead`/`RemoteKill`/
  `RemoteFeed` decode the handle and refuse it when the slot was
  released or reopened under a new token, so a telnetd pump task from an
  old session can never write into a different session that reused the
  slot (UAOS-53).
- Output: `inst_print`/`shell_print_raw` route through `remote_send()`,
  which IAC-escapes literal `0xFF` bytes (RFC 854) over
  `remote_send_raw()` before pushing bytes and marks
  the session dead if the socket dies (UAOS-51).  `remote_send_raw`
  sends through `tcp_conn_send(remote_sock, remote_gen, ...)` — the
  generation check keeps a live session from ever writing onto a
  different connection that reused the freed TCP slot (UAOS-263).
  `tcp_send` returns 0
  while a segment is still unacked or the peer window is closed
  (UAOS-55), so `remote_send_raw` polls the stack
  and retries with a ~60 s `g_pit_ticks` stall bound.  `clear` sends ANSI clear-screen; `endcli` sets
  `remote_dead` via the existing `close_shell` callback, which makes the
  session task exit and releases the slot.  `ShellWin_RemoteCleanupTask()`
  is hooked from `Task_Exit()` (UAOS-263): a remote shell task that dies
  without reaching `shell_task_entry`'s exit path still sets
  `remote_dead` and releases `remote_inuse`/`remote_sock` — otherwise
  the remote slot would stay allocated forever.  The socket itself is
  then reclaimed by the pump, which sees `RemoteIsDead` (a released
  handle decodes dead) and runs its normal close path.
- Input: `ShellWin_RemoteFeed()` enqueues NVT-decoded bytes into the
  instance's normal key ring (256 bytes, `SHELL_KB_BUFSIZE`) and returns
  whether every byte was accepted; when the ring is full it returns
  false and the telnetd pump retries the remainder on the next poll
  instead of dropping it — a dropped newline previously left the
  session waiting for line termination forever, making input bursts
  look like a hang (UAOS-52).
  `inst_handle_key` dispatches to a remote
  line editor that mirrors the window editor (history recall, cursor
  keys, tab completion, backspace, Home/End/Delete) but repaints with
  `\r` + `ESC[2K` + prompt + buffer instead of the framebuffer input bar.
  For the common case — a printable character appended at end of line —
  it echoes just that character instead of repainting the whole line,
  which keeps typing usable over a latency-bound TCP link
  (UAOS-58); mid-line edits, history recall, tab completion and
  Delete still trigger a full repaint.
- Remote command policy (UAOS-59): because telnetd is unauthenticated,
  `run_cmd` checks every command's basename against
  `k_remote_blocked`/`k_remote_logged` before any dispatch path —
  builtins, resident commands, the native registry, or PATH-exec'd
  binaries.  Framebuffer/desktop-only commands are refused with
  "not available on remote shells" (rc 20); destructive/system-wide
  commands run but are logged to klog/serial as
  `telnetd: remote shell ran '...'`.  `NativeCmdCtx.remote` marks a
  context as remote.
  Feeding `0x03` (Ctrl-C — also produced by Telnet `IAC IP`/`IAC AO`)
  sets the instance's `break_req` flag and signals the session task
  with `SIGF_BREAKF` so command waits, `read_line`/`read_key` and
  `Wait(SIGF_CHILD)` wake early; the flag is consumed by the dispatch
  loop, script runner and `FOR` loops, which abort with `***Break`,
  and a `dispatch_broken` marker propagates consumed breaks to
  enclosing loops (UAOS-50).
- Full-screen inline editors (`vim`, `ed`) and other WM-only modes are
  refused on remote sessions.
- `ShellWin_RemoteIsDead`/`ShellWin_RemoteKill` let the daemon detect a
  finished session and force-terminate one whose peer went away.
  `RemoteKill` also signals the session task with `SIGF_BREAKF`
  (UAOS-287): the foreground-child `Wait(SIGF_CHILD|SIGF_BREAKF)` loops
  bail on `remote_dead`, so a remote shell parked inside a blocking
  command (e.g. `more` waiting on a key) wakes and tears down instead
  of pinning the slot.  A session task that exits while a pipe stage
  is still running is handled by per-shell pipe state (see below).
- Spawned-command input (UAOS-287): x86-64 tasks launched by a shell
  carry `UaosTask::key_src` pointing at that `ShellInstance`, inherited
  through `sys_spawn`.  `sys_read`/`sys_readkey` route through
  `ShellWin_KeySrcGet(key_src)`, which pops this instance's `kb` ring —
  the same queue `RemoteFeed` fills — so interactive userspace commands
  (`more`, `dir KEYS/INTER`) read telnet keystrokes rather than the
  physical PS/2 keyboard.  On a dead remote it returns -1 so the child
  exits; tasks with `key_src == NULL` keep the old PS/2 path.
- Pipe state is per-shell (UAOS-287): the pipeline input filename /
  active flag live on the `ShellInstance` (`pipe_in_file` /
  `pipe_in_active`) rather than globals, and the `T:pipeN` temp name is
  derived from the shell slot index, so two shells — or a dead session's
  abandoned pipe — can't inject a stale `T:pipe0` argument into another
  shell's next command.  `NativeCmdCtx.pipe_file` and the argv
  augmentation in the X64-bin launcher both read the instance fields.

## Debug Logging

Hot-path serial logging is compile-time gated to avoid UART busy-wait overhead (each char causes a TCG vmexit) and PIT tick skew (logs run at IRQ time with IF=0):

- `WM_DEBUG` (in `wm.c`, default 0) — gates `WM_LOG`/`WM_LOG_DEC`.
- `DT_DEBUG` (in `desktop.c`, default 0) — gates `DT_LOG`/`DT_LOG_DEC`.
- `FB_DEBUG` (in `filebrowser.c`, default 0) — gates `FB_LOG` and the on-screen debug overlay.
- `MOUSE_DEBUG` (in `ps2mouse.c`, default 0) — gates the per-packet serial dump in `PS2Mouse_IRQHandler`.

Set any to 1 to re-enable the corresponding debug output. Boot-time logs are unaffected.

The `inst_print()`/`shell_print_raw()` serial mirror (every shell line echoed to COM1) is gated at runtime by `klog_enabled(KLOG_SHELL, KLOG_TRACE)` rather than a compile-time flag — it is off by default (default threshold `KLOG_DEBUG`) because a polled 115200-baud UART costs ~86 µs per character and dominated remote-session output. Re-enable it live with `klog shell=trace`.
