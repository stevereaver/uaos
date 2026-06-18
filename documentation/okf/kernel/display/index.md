---
type: Kernel Subsystem
title: Display and Window Manager
description: The UAOS graphical environment, including the linear framebuffer and windowing system.
resource: /kernel/display/
tags: [display, wm, framebuffer, gui]
timestamp: 2026-06-18T10:00:00Z
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
- **Dragging & Resizing**: Title bars can be dragged to move windows, and some windows support resizing.
- **Event Routing**: Mouse and keyboard events are routed to the active window's callbacks.

### Window Callbacks
Each window provides callbacks for:
- `draw`: Redrawing the client area.
- `on_key`: Handling keystrokes.
- `on_click`: Handling mouse clicks in the client area.

## Workbench Elements
- **Backdrop**: Stipple pattern backdrop.
- **Menu Bar**: Fixed at the top of the screen.
- **Icons**: Desktop icons representing disks and tools.
