---
type: Kernel Library
title: icon.library
description: UAOS native implementation of the AmigaOS icon.library (DiskObject, tooltypes) and Workbench launch semantics (WBStartup) for M68k tasks.
resource: /kernel/exec/icon_lib.c
tags: [workbench, library, m68k, icons, wbstartup, launch]
timestamp: 2026-10-08T02:21:00Z
---

# icon.library + Workbench launch (UAOS-253)

`icon.library` serves guest `DiskObject`s built from **real classic Amiga `.info` files**, and the Workbench launch path makes icon double-clicks behave like AmigaOS Workbench launches rather than CLI launches.

## Key Files

- `kernel/exec/icon_lib.c` / `icon_lib.h` — real `.info` parser/serialiser, guest `DiskObject` builder, LVO handlers.
- `kernel/shell/exec_file.c` — `ExecFile_RunWB()` launcher (tool/project icons, extra WBArgs).
- `kernel/exec/exec_task.c` — `g_wb_pending` handoff; `UaosTask.m68k_wb` snapshot; teardown reply check.
- `emulation/uaos_m68k_glue.c` — `UAOS_Emu_SetupWbLaunch()` guest environment (PROC_ENV region extension).
- `kernel/display/filebrowser.c` — double-click → `ExecFile_RunWB`; shift-click multi-select.
- `kernel/display/desktop.c` — Leave-Out icon double-click → `ExecFile_RunWB`.
- `kernel/shell/cmd_wbrun.c` — `C:wbrun` host-side entry point.
- `kernel/dos/icon_loader.c` — `Icon_Load`/`Icon_Save`/`Icon_SavePosition` now use the real format via `Icon_ParseBuf`.

## Real .info format

The classic file is a 78-byte `DiskObject` header (magic `0xE310`, 44-byte embedded `Gadget`, presence-flag pointer fields, `CurrentX/Y`, `StackSize` — `pack(2)`, same layout in memory) followed positionally by: `DrawerData` (56 B), normal `Image` (20 B header + word-aligned planes ×depth), selected `Image`, default tool, tooltype block (`u32` = `(count+1)*4`), tool window, optional `DrawerData2` tail. Strings are `u32` length-prefixed including NUL. On-disk pointer fields are stale/marker values, not offsets.

`Icon_ParseBuf()` extracts metadata (`IconMeta`) including file offsets of the variable sections; `build_guest_dobj()` copies the 78-byte header verbatim into one guest allocation, patches the pointer fields to real guest addresses, and appends `DrawerData`(62 B)/images/`do_ToolTypes` STRPTR array/C strings.

## Implemented LVOs

| LVO | Function | Notes |
|---|---|---|
| -78 | `GetDiskObject` | Parses `<name>.info` (cwd-relative names allowed) into a guest `DiskObject` |
| -84 | `PutDiskObject` | Serialises the guest `DiskObject` back to real `.info` via VFS |
| -90 | `FreeDiskObject` | Releases the single guest allocation |
| -96 | `FindToolType` | Case-insensitive key match over `do_ToolTypes` |
| -102 | `MatchToolValue` | `|`-separated alternative matching |
| -108 | `BumpRevision` | Copies icon to `Copy_of_<name>.info` |
| -120/-126 | `GetDefDiskObject`/`PutDefDiskObject` | Minimal guest DiskObject / accept-discard |
| -132 | `GetDiskObjectNew` | Alias of GetDiskObject (no default-image fallback) |
| -138 | `DeleteDiskObject` | VFS delete of `<name>.info` |

Registration rides the `LIB_ROM` generated-base machinery (`UAOS_ICON_Register` in `rom_modules.c`), so `OpenLibrary("icon.library", v)` returns a per-task generated base.

## Workbench launch semantics

`ExecFile_RunWB(path, extra_paths, n)` is entered by the filebrowser/desktop/`wbrun` instead of `ExecFile_Run`:

- Parses the clicked icon's `.info` (`IconMeta`): type, stack size, tool window, tooltypes.
- `WB_PROJECT` icons resolve `do_DefaultTool` (absolute, or relative to the project's drawer) and enqueue the project as `WBArg[1]`.
- Extra paths (shift-click multi-selection) append as further WBArgs; `WBArg[0]` is always the tool itself.
- Fills `g_wb_pending` (consumed once by `Task_CreateM68k` into `task->m68k_wb`), sets `g_uaos_cwd` to the tool's drawer, and creates the M68k task.

`UAOS_Emu_SetupWbLaunch()` then builds a Process with `pr_CLI = 0`, a real `pr_MsgPort`, `pr_CurrentDir` = `dos_LockPath_glue(tool_dir)`, `pr_StackSize` from the icon, and a queued 40-byte `WBStartup` (`sm_Segment` = loaded seglist BPTR, `sm_NumArgs`, `sm_ArgList`, `sm_ToolWindow`, `sm_Process` = `&pr_MsgPort`). The reply port is a fake Workbench Process at `PROC_ENV_BASE+0x800`; at task teardown `UAOS_Emu_WbStartupReplied()` reports whether the guest `ReplyMsg`ed. Entry registers follow WB convention (A0=NULL, D0=0).

Shell/CLI launches (`ExecFile_Run`, `wbrun`-less commands) are unchanged — `UAOS_Emu_SetupProcess` still builds the CLI environment.

## Tests

`system/Demos/src/WBLaunchTest.s` (+ `system/Demos/WBLaunchTest.info`, `WBProj[.info]` fixtures) and `tests/qemu_wblaunch_test.py` cover: `pr_CLI == 0`, WBStartup receipt, `sm_NumArgs`/`sm_Segment`/`sm_ArgList`, `GetDiskObject`+`FindToolType`+`MatchToolValue`, `PutDiskObject` → `RAM:WBPUT.info`, `ReplyMsg`, and project-icon default-tool launch with `sm_NumArgs = 2`.
