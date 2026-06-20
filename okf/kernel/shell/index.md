---
type: Kernel Subsystem
title: UAOS Shell
description: The command-line interface for UAOS.
resource: /kernel/shell/
tags: [shell, cli, commands]
timestamp: 2026-06-20T15:30:00Z
---

# UAOS Shell

The UAOS Shell provides a command-line interface for interacting with the system. It is implemented as a resident command handler and a shell window.

## Shell Window

The Shell window (`shell_win.c`) is a graphical window managed by the WM. It provides:
- Scrollable history.
- Line-editing for input.
- Output redirection to the framebuffer (with line buffering and background flushing).
- Synchronous child execution tracking (blocking on `SIGF_CHILD` when running foreground tasks).

## Command Execution

Commands in UAOS can be:
- **Internal**: Built into the shell (e.g., `alias`, `set`).
- **Resident**: Compiled into the kernel but executed as separate logic (e.g., `dir`, `mem`).
- **External**: Loaded from disk. This includes:
  - **M68k Amiga Hunk binaries** wrapped with a custom 32-byte UAOS header and executed via the CPU emulator.
  - **Native x86-64 ELF64 binaries** compiled using the `-nostdlib` flag and executed as Ring-3 tasks (e.g., `C:pwd`, `C:find`, `C:file`, `C:strings`).

> [!NOTE]
> Previously resident commands (like `pwd`) have been refactored into external userspace utilities to utilize and test the Ring-3 system call interface.

## File Browser Launching

Double-clicking an icon in the Workbench file browser (`filebrowser.c`) calls `ExecFile_Run()` in `exec_file.c`. The loader reads the first four bytes of the selected file to determine whether it is a UAOS wrapper binary (e.g. `UAOS` for a native command), an Amiga Hunk executable, or an ELF64 binary. Native commands are dispatched through `NativeCmd_Run()`, which constructs a minimal `NativeCmdCtx` and invokes the corresponding `Cmd_` function registered in the native command table.

## Scripting

The shell supports basic scripting via `S:Startup-Sequence` and the `execute` command, allowing for automated system initialization.
