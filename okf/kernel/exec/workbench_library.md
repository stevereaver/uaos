---
type: Kernel Library
title: workbench.library
description: UAOS native implementation of the AmigaOS workbench.library for emulated M68k tasks.
resource: /kernel/exec/workbench_lib.c
tags: [workbench, library, m68k, thunking, desktop]
timestamp: 2026-06-24T17:00:00Z
---

# workbench.library

`workbench.library` provides AmigaOS Workbench integration to emulated M68k tasks. The UAOS implementation is a native thunk layer that tracks application icons and windows and bridges them to the host desktop and VFS layers.

## Key Files

- `kernel/exec/workbench_lib.c` — native implementations and function table.
- `kernel/exec/workbench_lib.h` — guest structure layouts and constants.
- `emulation/uaos_m68k_glue.c` — M68k LVO stub installation and dispatch.

## Implementation Status

| Function | Status | Notes |
|---|---|---|
| `AddAppIcon` | Implemented | Registers an application icon in the internal app-icon table (max 16). |
| `RemoveAppIcon` | Implemented | Removes an application icon and frees its slot. |
| `AddAppWindow` | Implemented | Registers an application window in the internal app-window table (max 16). |
| `RemoveAppWindow` | Implemented | Removes an application window and frees its slot. |
| `GetNextIcon` | Implemented | Enumerates icons from the VFS/desktop layer. |
| `WorkbenchControl` | Implemented / Stub | Parses tag list; some tags are stored for introspection. |
| `BeginRefresh` | Stub | Reserved for future damage handling. |
| `EndRefresh` | Stub | Reserved for future damage handling. |

## Guest Pointer Model

Because UAOS does not run a full AmigaOS Workbench process, `workbench.library` returns synthetic guest pointers for app icons and app windows:

- App icon handles are allocated from the `0x01000000` range.
- App window handles are allocated from the `0x02000000` range.

These synthetic addresses are unique IDs that the native implementation maps back to its internal tables. The guest application sees a valid non-NULL pointer and can pass it back to `RemoveAppIcon`, `RemoveAppWindow`, or other functions.

## Integration

- **Desktop**: `AddAppIcon` creates an entry that the host desktop can query through `GetNextIcon()`. The desktop's file browser and icon rendering code can treat these entries like native disk icons.
- **VFS**: Icon enumeration and file/drawer information come from the VFS layer (`kernel/dos/vfs.c`), so app icons reflect the actual filesystem state.
- **Intuition**: App windows are associated with host WM windows where appropriate; the Workbench layer tracks them so that the desktop can query active application windows.

## Use Cases

`workbench.library` is primarily used by Amiga applications that want to add their own icon to the Workbench or open an application window that participates in the desktop environment. The implementation provides enough compatibility for simple icon and window registration while delegating actual rendering and input to the native desktop and window manager.
