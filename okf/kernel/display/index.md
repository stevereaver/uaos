---
type: Kernel Subsystem
title: Display and Window Manager
description: The UAOS graphical environment, including the linear framebuffer and windowing system.
resource: /kernel/display/
tags: [display, wm, framebuffer, gui]
timestamp: 2026-06-24T17:00:00Z
---

# Display and Window Manager

UAOS provides a graphical user interface (GUI) inspired by the Amiga Workbench. It operates directly on a linear framebuffer.

## Linear Framebuffer

The framebuffer is initialized during boot via Multiboot2 tags. UAOS supports 32-bit color (BGRA/RGBA) and provides primitives for:
- Drawing pixels, lines, and rectangles.
- Rendering bitmaps and icons.
- Font rendering (8x16 bitmap fonts).

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

## Workbench Elements
- **Backdrop**: Solid Amiga grey (`WB_GREY`, R:170 G:170 B:170).
- **Menu Bar**: Fixed at the top of the screen.
- **Icons**: Desktop icons representing disks and tools.

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
Desktop icons can be selected with a single click. The selected icon is rendered using one of two methods:

1. **Selected planar image**: If the `.info` file contains a dedicated selected-state image, that image is drawn directly.
2. **Inverse/video fallback**: If no selected image is present, the normal icon is drawn with inverted RGB colours (preserving transparency).

The `IconImage` structure tracks whether a selected planar image was actually loaded (`has_selected`). The renderer (`icon_render.c`) chooses the appropriate method, and the desktop (`desktop.c`) tracks which icon is currently selected and requests a redraw when the selection changes.

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
