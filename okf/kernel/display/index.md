---
type: Kernel Subsystem
title: Display and Window Manager
description: The UAOS graphical environment, including the linear framebuffer and windowing system.
resource: /kernel/display/
tags: [display, wm, framebuffer, gui]
timestamp: 2026-08-26T12:00:00Z
---

# Display and Window Manager

UAOS provides a graphical user interface (GUI) inspired by the Amiga Workbench. It operates directly on a linear framebuffer.

## Linear Framebuffer

The framebuffer is initialized during boot via Multiboot2 tags. UAOS supports 32-bit color (BGRA/RGBA) and provides primitives for:
- Drawing pixels, lines, and rectangles.
- Rendering bitmaps and icons.
- Font rendering (8x16 and 8x8 bitmap fonts).

### Double Buffering and Dirty-Rect Tracking

All WM-driven rendering goes through a back buffer (`g_backbuf`, 1280×1024 max in BSS). The pipeline is:

1. `FB_BeginDraw()` — switches primitives to back-buffer mode and resets the dirty rectangle.
2. Primitives (`FB_FillRect`, `FB_DrawHLine`, `FB_DrawVLine`, `FB_PutChar`, `FB_PutCharSmall`, `FB_BlitARGB`, `FB_PutPixel`) paint into the back buffer and extend a bounding-box dirty rectangle (`g_dirty_x0/y0/x1/y1`).
3. `Cursor_Redraw()` draws the cursor sprite into the back buffer (its pixels are included in the dirty rect automatically).
4. `FB_Flip()` — `memcpy`s only the dirty rows from the back buffer to VRAM (32bpp uses row `memcpy`; 24bpp uses a per-pixel loop). If nothing changed, the flip is a no-op.

Direct-mode drawing (when `FB_IsDrawing()` is false) writes straight to VRAM and bypasses dirty tracking. `FB_DirtyInclude()` lets callers that touch VRAM directly during a back-buffered frame extend the dirty box.

### Fast Row-Based Primitives

The hot primitives (`FB_FillRect`, `FB_DrawHLine`, `FB_DrawVLine`, `FB_PutChar`, `FB_PutCharSmall`) hoist the `g_drawing` and `bpp` branches out of the per-pixel loop, resolve the target row pointer once, and run a tight inner loop. `FB_BlitARGB` provides a clipped ARGB row blit (alpha-keyed, optional colour inversion) used by `icon_render.c` instead of per-pixel `FB_PutPixel` calls.

### Mode-Size Clamp

`FB_Init` clamps `g_fb.width`/`g_fb.height` to the back-buffer dimensions (`BB_MAX_W` × `BB_MAX_H` = 1280×1024) so the desktop always lays out inside the drawable region even if GRUB selects a larger mode.

### String Clipping

`FB_PutStrCentred` and `FB_PutStrSmallCentred` truncate the string to fit the target rectangle (character-granular), preventing long window titles from overdrawing the zoom/depth gadgets or bleeding past the window edge.

## Window Manager (WM)

The Window Manager (`wm.c`) manages a z-ordered stack of windows. It handles user interaction and repainting.

### Key Features
- **Z-Order Management**: Windows are stacked, with the top window receiving focus.
- **Click-to-Focus**: Clicking a window title bar or client area raises it to the top.
- **Raise / Lower**: `WM_RaiseWindow` brings a window to the front; `WM_LowerWindow` sends it to the back. `WM_MoveWindowInFrontOf` and the depth gadget also reorder the z-stack. Whenever the z-order of a window changes, `UAOS_Intuition_NotifyDepthChange()` is called so windows with `WA_NotifyDepth` can receive `IDCMP_NEWSIZE`.
- **Repaint Requests**: `WM_RepaintWindow` requests a chrome/content redraw of a window (currently performed as a full-scene redraw to handle overlap correctly).
- **Title Changes**: `WM_SetWindowTitle` updates the title string stored in the `WmWindow` and triggers a full redraw so the title bar is refreshed.
- **Dragging & Resizing**: Title bars can be dragged to move windows, and some windows support resizing.
- **Event Routing**: Mouse and keyboard events are routed to the active window's callbacks.

### Window Callbacks
Each window provides callbacks for:
- `draw`: Redrawing the client area.
- `on_key`: Handling keystrokes.
- `on_click`: Handling mouse clicks in the client area.

### Close and Depth Gadgets

`WM_CloseWindow` repaints via the double-buffered `WM_Redraw()` path (no flicker). The depth gadget (`depth_window`) reorders the z-stack and notifies Intuition of the focus change via `wm_notify_focus_change()` so `IDCMP_ACTIVEWINDOW`/`INACTIVEWINDOW` are sent.

## Software Cursor

The software cursor (`cursor.c`) uses save/restore of background pixels for flicker-free movement. Key rules:

- **IRQ-time moves**: `Cursor_Move` is called from `PS2Mouse_IRQHandler`. If a back-buffered frame is in progress (`FB_IsDrawing()`), it only updates the stored position — the frame-end `Cursor_Redraw` paints the cursor at the new position. This prevents back-buffer pollution and ghost-cursor artifacts.
- **Sprite scaling**: The 32×32 and 48×48 arrow pointers are generated at boot by integer-scaling the verified 16×16 sprite (2× and 3× respectively). The previous hand-typed tables had wrong per-row element counts and produced skewed sprites.
- **Background save/restore**: `cursor_save_bg` reads via `FB_GetPixel` (back buffer when drawing, VRAM otherwise — both authoritative after the last flip). `cursor_restore_bg` is a no-op during back-buffered drawing since the full frame is repainted.

## Workbench Elements
- **Backdrop**: Solid Amiga grey (`WB_GREY`, R:170 G:170 B:170).
- **Menu Bar**: Fixed at the top of the screen.
- **Icons**: Desktop icons representing disks and tools.

### Icon Cache

The desktop icon list (including `.info` file loading and planar decoding) is cached in `get_icons()` and only rebuilt when the VFS mount table changes (mount count or any mount name differs from the cached fingerprint). This avoids reloading every `.info` from VFS on every frame and mouse event. Click/selection state persists in the cached `icons[]` array across calls.

### Clock Redraw

The menubar clock displays `HH:MM`. `Desktop_UpdateClock` (called once per second from IRQ context) only sets the redraw-pending flag when the displayed minute changes, not every second. `Desktop_FlushClockRedraw` (called from the main loop) consumes the flag and calls `WM_Redraw()`.

### Menu Bar

The menu bar displays the Workbench 3.x-style menu titles (`Workbench`, `Window`, `Icons`, `Tools`, `Shell`, `UAOS`) plus a clock on the right. The menus follow the classic Amiga press-and-drag behaviour:

1. **Press and hold** the right mouse button on a menu title to open its drop-down.
2. **Drag** the cursor over the items to highlight them; the highlight updates as the cursor moves.
3. **Release** the right mouse button on an item to trigger its action and close the menu.
4. Dragging the cursor over another menu title while the right button is held switches to that menu.
5. A left mouse click dismisses an open menu without triggering an action.

#### Workbench Drop-down Menu

The `Workbench` menu contains the following items:

| Item | Action |
|------|--------|
| Backdrop | Stub — intended to control backdrop visibility. |
| Execute Command | Opens the Shell window. |
| Redraw All | Requests a full desktop/window repaint. |
| Update All | Stub — intended to refresh all desktop icons. |
| Last Message | Stub — intended to replay the last system message. |
| About | Opens the About window. |
| Quit | Stub — intended to shut down the Workbench session. |

#### Window Drop-down Menu

The `Window` menu contains the following items (currently all stubbed):

| Item | Action |
|------|--------|
| New Drawer | Stub — intended to create a new drawer. |
| Open Drawer | Stub — intended to open the selected drawer. |
| Close | Stub — intended to close the active window. |
| Update | Stub — intended to refresh the active window. |
| Select Contents | Stub — intended to select all items in a window. |
| Clean Up | Stub — intended to tidy icon positions. |
| Snapshot | Stub — intended to save window/icon positions. |
| Show | Stub — intended to show hidden items. |
| View By | Stub — intended to change the window view mode. |

#### Icons Drop-down Menu

The `Icons` menu (opened with a right-click on the `Icons` title) contains the following items. A horizontal divider separates the upper icon-management items from the lower destructive/utility items. All items are currently stubbed.

| Item | Action |
|------|--------|
| Copy | Stub — intended to copy selected icons. |
| Rename | Stub — intended to rename selected icons. |
| Information | Stub — intended to show icon information. |
| Snapshot | Stub — intended to save icon positions. |
| Unsnapshot | Stub — intended to remove saved icon positions. |
| Leave Out | Stub — intended to leave icons on the desktop. |
| Put Away | Stub — intended to put icons back in their drawers. |
| *divider* | Horizontal separator. |
| Delete | Stub — intended to delete selected icons. |
| Format | Stub — intended to format a disk. |
| Empty Trash | Stub — intended to empty the trash. |

#### Tools Drop-down Menu

The `Tools` menu (between `Icons` and `Shell`) contains the following item:

| Item | Action |
|------|--------|
| Reset WB | Stub — intended to reset the Workbench session. |

The menus are rendered by `desktop.c` and managed through a small internal state (`g_menu_index`, `g_menu_hover`, etc.). Menu items support a divider flag (`is_divider`) for separator lines. The window manager tracks both left and right mouse buttons and forwards desktop events and hover tracking to highlight items and dispatch the selected action.

### Icon Selection State
Desktop icons can be selected with a single click or by lasso (rubber-band) drag. The selected icon is rendered using one of two methods:

1. **Selected planar image**: If the `.info` file contains a dedicated selected-state image, that image is drawn directly.
2. **Inverse/video fallback**: If no selected image is present, the normal icon is drawn with inverted RGB colours (preserving transparency).

The `IconImage` structure tracks whether a selected planar image was actually loaded (`has_selected`). The renderer (`icon_render.c`) chooses the appropriate method, and the desktop (`desktop.c`) tracks which icons are currently selected and requests a redraw when the selection changes.

### Lasso (Rubber-band) Selection
Dragging the left mouse button on the empty desktop backdrop activates lasso selection — a dashed black rectangle follows the cursor (the "marquee"). Any icon whose bounding box (`ICON_W` x `ICON_H`) intersects the lasso rectangle is selected; icons outside the rectangle are deselected. The selection updates dynamically as the drag proceeds. On button release, the lasso rectangle disappears but the selection is retained.

A lasso drag (where the cursor moved) is not counted as a desktop click, so it does not contribute to the double-click-to-open-Shell counter. Only a click on empty desktop without dragging counts toward the double-click.

This matches classic Workbench 3.x behaviour. The lasso state is tracked in `desktop.c` (`g_lasso_active`, `g_lasso_start_x/y`, `g_lasso_cur_x/y`, `g_lasso_moved`) and the dashed border is drawn by `draw_lasso()` after icons but before the menu dropdown and bars, clipped to the desktop backdrop area (between the menu bar and status bar).

### File Browser Lasso Selection
Lasso selection is also available inside drawer windows (`filebrowser.c`). Dragging the left mouse button on empty space within a browser's icon grid area (below the path bar) activates a lasso rectangle. Any icon cell that intersects the lasso is selected. The browser uses a per-icon `selected[]` array for multi-selection, replacing the previous single-`selected_icon` model. Single-clicking an icon selects only that icon and cancels any active lasso. The lasso rectangle is clipped to the browser's client area below the path bar.

## Application Windows

The display layer includes several Workbench-style application windows in addition to the file browser and shell:

- **About window (`about_win.c`)**: Shows UAOS version, build date, display resolution, and memory size.
- **Calculator (`calc_win.c`)**: Amiga-style four-function calculator with double-precision arithmetic.
- **Clock (`clock_win.c`)**: Digital time and date display, updated once per second from the RTC.
- **Network Info (`netinfo_win.c`)**: Displays interface IP, MAC, gateway, DNS, and DHCP status.
- **Vim Editor (`vim_win.c`)**: Modal text editor with Normal/Insert/Visual/Command modes, search, undo, and `S:vim.conf` configuration.
- **Pointer Preferences (`pointer_prefs.c`)**: Cursor size, double-pixel mode, and acceleration settings.
- **Userspace GUI Window (`user_window.c`)**: Backing for native Ring-3 programs that use the GUI syscall interface.

## Shell Window

`shell_win.c` implements the graphical CLI window. It provides scrollable history, input line editing, output buffering, and synchronous child tracking. It can host multiple independent shell instances and dispatches commands to the native command table, resident commands, external ELF64 userspace programs, or embedded M68k binaries.

## Debug Logging

Hot-path serial logging is compile-time gated to avoid UART busy-wait overhead (each char causes a TCG vmexit) and PIT tick skew (logs run at IRQ time with IF=0):

- `WM_DEBUG` (in `wm.c`, default 0) — gates `WM_LOG`/`WM_LOG_DEC`.
- `DT_DEBUG` (in `desktop.c`, default 0) — gates `DT_LOG`/`DT_LOG_DEC`.
- `MOUSE_DEBUG` (in `ps2mouse.c`, default 0) — gates the per-packet serial dump in `PS2Mouse_IRQHandler`.

Set any to 1 to re-enable the corresponding debug output. Boot-time logs and shell serial mirroring are unaffected.
