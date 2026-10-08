---
type: Kernel Concept
title: Amiga .info Icon Loading & Rendering
description: How UAOS parses classic planar Amiga .info icons and renders normal and selected states.
resource: /kernel/exec/icon_def.h, /kernel/dos/icon_loader.c, /kernel/display/icon_render.c
tags: [display, icons, workbench, gui]
timestamp: 2026-10-08T02:21:00Z
---

# Amiga .info Icon Loading & Rendering

UAOS supports classic Amiga Workbench `.info` icon files. These files contain one or two planar bitmapped images: a **normal** image and an optional **selected** image.

## Data Structures

- `IconImage` — stores width, height, depth, a `has_selected` flag, and two ARGB buffers (`normal` and `selected`).
- `ParsedIcon` — the native representation used by the kernel, including label, type, position, and the `IconImage` data.

## Loading Flow

`Icon_Load()` in `kernel/dos/icon_loader.c` reads the `.info` file from the VFS via the shared real-format parser (`Icon_ParseBuf`/`Icon_ReadInfo` in `kernel/exec/icon_lib.c`, UAOS-253):

1. Parses the classic 78-byte `DiskObject` header (magic `0xE310` + embedded 44-byte `Gadget`) and validates the magic. On-disk pointer fields are presence flags; optional sections (`DrawerData`, `Image` records, default tool, tooltype block, tool window) follow sequentially at computed offsets.
2. Reads the normal `Image` (20-byte header + planar data) and converts bitplanes to chunky ARGB.
3. Reads the optional selected `Image`.
   - If the selected image matches the normal image dimensions, it is converted and `has_selected` is set to `1`.
   - Otherwise the `selected` buffer is filled with a copy of the normal image and `has_selected` remains `0`.
4. Extracts `type`, position (`CurrentX/Y` — signed 32-bit at offsets 58/62), `default_tool`, `tool_types`, `stack_size`, and `tool_window` into `ParsedIcon`.

### Pen Mapping

Classic `.info` images carry no palette — planar pen indices map onto the Workbench 2.x/3.x screen palette: pen 0 renders transparent (backdrop shows through), pen 1 = black, pen 2 = white, pen 3 = the live `WB_BLUE` (`#3B67A2`). Pens 4-7 in depth-3 images use the WB 3.x eight-colour extension set (`#7B7B7B`, `#AFAFAF`, `#AA907C`, `#FFA997`). `Icon_Save()`'s `argb_to_pen` is the inverse of this table. `Icon_MakeDefault()` procedural fallbacks draw with `WB_WHITE`/`WB_BLACK`/`WB_GREY` so they track palette changes too.

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
- Shift-click toggles additional icons into the selection (Amiga multi-select).
- Clicking the path bar or an empty area deselects the current icon.
- A double-click opens a drawer or executes a file **with Workbench launch semantics** (UAOS-253): the file's `.info` supplies stack size and tooltypes; project icons launch their `do_DefaultTool` with the project as `WBArg[1]`; other selected icons become extra `WBArg`s. The guest sees `pr_CLI == 0` and a queued `WBStartup` message — see [icon.library](/kernel/exec/icon_library.md).
