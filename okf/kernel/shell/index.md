---
type: Kernel Subsystem
title: UAOS Shell
description: The command-line interface for UAOS.
resource: /kernel/shell/
tags: [shell, cli, commands]
timestamp: 2026-06-24T17:00:00Z
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
- **Internal**: Built into the shell (e.g., `alias`, `set`, `cd`, `path`, `prompt`, `failat`, `why`).
- **Resident**: Compiled into the kernel but executed as separate logic (e.g., `dir`, `mem`, `libs`).
- **External**: Loaded from disk. This includes:
  - **M68k Amiga Hunk binaries** wrapped with a custom 32-byte UAOS header and executed via the CPU emulator.
  - **Native x86-64 ELF64 binaries** compiled using the `-nostdlib` flag and executed as Ring-3 tasks (`C:pwd`, `C:find`, `C:file`, `C:strings`, `C:Guide`).
- **Native C: commands**: Executed in-place by the kernel command dispatcher (`cmd_*.c` in `kernel/shell/`).

> [!NOTE]
> Some previously resident commands (like `pwd`) have been refactored into external Ring-3 userspace utilities to exercise the `INT 0x80` syscall interface. Others remain as native C: commands implemented directly in the kernel.

## Command Reference

The following native C: commands are implemented in `kernel/shell/`:

| Category | Commands |
|---|---|
| **Filesystem** | `dir`, `copy`, `delete`, `rename`, `makedir`, `type`, `more`, `protect`, `attr`, `filenote`, `search`, `sort`, `join`, `grep` |
| **Volume / Disk** | `info`, `disks`, `diskchange`, `mount`, `format`, `fdisk`, `addbuffers`, `relabel`, `install` |
| **System** | `version`, `mem`, `avail`, `status`, `info`, `libs`, `ps`, `jobs`, `wait`, `changetaskpri`, `stack`, `why`, `failat`, `quit`, `endcli`, `newcli`, `execute`, `resident`, `stacktrace`, `strace` |
| **Network** | `ifconfig`, `route`, `ping`, `nslookup`, `ntpd`, `netstart`, `netstop`, `netinfo` |
| **Desktop / Windows** | `loadwb`, `calc`, `clock`, `pointer`, `vim`, `requestchoice`, `requestfile` |
| **Utilities** | `echo`, `date`, `time`, `ask`, `which`, `getenv`, `setenv`, `unset`, `alias`, `unalias`, `path`, `prompt`, `clear`, `echo` |

For full syntax and examples, see the `README.md` and `documentation/Dos_Manual.md` in the repository root.

## File Browser Launching

Double-clicking an icon in the Workbench file browser (`filebrowser.c`) calls `ExecFile_Run()` in `exec_file.c`. The loader reads the first four bytes of the selected file to determine whether it is a UAOS wrapper binary (e.g. `UAOS` for a native command), an Amiga Hunk executable, or an ELF64 binary. Native commands are dispatched through `NativeCmd_Run()`, which constructs a minimal `NativeCmdCtx` and invokes the corresponding `Cmd_` function registered in the native command table.

## Scripting

The shell supports basic scripting via `S:Startup-Sequence` and the `execute` command, allowing for automated system initialization.
