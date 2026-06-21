---
type: Kernel Concept
title: Amiga .info Icon Loading & Rendering
description: How UAOS parses classic planar Amiga .info icons and renders normal and selected states.
resource: /kernel/exec/icon_def.h, /kernel/dos/icon_loader.c, /kernel/display/icon_render.c
tags: [display, icons, workbench, gui]
timestamp: 2026-06-21T11:25:00Z
---

# Amiga .info Icon Loading & Rendering

UAOS supports classic Amiga Workbench `.info` icon files. These files contain one or two planar bitmapped images: a **normal** image and an optional **selected** image.

## Data Structures

- `IconImage` — stores width, height, depth, a `has_selected` flag, and two ARGB buffers (`normal` and `selected`).
- `ParsedIcon` — the native representation used by the kernel, including label, type, position, and the `IconImage` data.

## Loading Flow

`Icon_Load()` in `kernel/dos/icon_loader.c` reads the `.info` file from the VFS:

1. Parses the `DiskObject` header and validates the magic `0xE310`.
2. Reads the normal `WBImage` dimensions and converts planar bitplanes to chunky ARGB.
3. Reads the optional selected `WBImage`.
   - If the selected image matches the normal image dimensions and fits in the file, it is converted and `has_selected` is set to `1`.
   - If the selected image is missing or mismatched, the `selected` buffer is filled with a copy of the normal image and `has_selected` remains `0`.

## Rendering

`Icon_Draw()` and `Icon_DrawSelected()` in `kernel/display/icon_render.c` blit ARGB pixels to the linear framebuffer, skipping fully transparent pixels.

### Selected State

`Icon_DrawSelected()` chooses the rendering method based on `has_selected`:

- **Selected image present**: draw the embedded selected image directly.
- **Selected image absent**: draw the normal image with inverse/video colours (`RGB ^ 0xFFFFFF`) while preserving the alpha channel.

Procedural fallback icons (e.g., the default disk icon on the desktop and the small folder/file icons in the file browser) also use inverse/video colours when selected.

The desktop (`kernel/display/desktop.c`) tracks the currently selected icon via `IconState.is_selected` and calls the appropriate draw function. The file browser (`kernel/display/filebrowser.c`) tracks `selected_icon` and inverts colours for the selected drawer or tool icon.

## Interaction

### Desktop Icons

- A single click on a desktop icon selects it and deselects all other icons.
- Clicking the desktop backdrop deselects all icons.
- A double-click on a desktop icon opens the file browser.

### File Browser Icons

- A single click on a drawer or tool icon selects it.
- Clicking the path bar or an empty area deselects the current icon.
- A double-click opens a drawer or executes a file.
