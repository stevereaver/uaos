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
- **Resident**: Compiled into the kernel but executed as separate logic (e.g., `mem`, `libs`, `version`).
- **External**: Loaded from disk. This includes:
  - **M68k Amiga Hunk binaries** wrapped with a custom 32-byte UAOS header and executed via the CPU emulator.
  - **Native x86-64 ELF64 binaries** compiled using the `-nostdlib` flag and executed as ring-0 tasks (for VirtualBox NEM compatibility). These use the `INT 0x80` syscall ABI to interact with the kernel.
- **Native C: commands**: Executed in-place by the kernel command dispatcher (`cmd_*.c` in `kernel/shell/`).

> [!NOTE]
> As of Phase 7, the following DOS commands have been migrated from kernel-resident native C: stubs to on-disk x86-64 ELF64 userspace binaries: `echo`, `type`, `dir`, `list`, `makedir`, `delete`, `rename`, `copy`, `protect`, `attr`, `grep`, `sort`, `join`, `search`, `filenote`, `more`. These binaries live in `system/userspace/` and use the shared helpers in `system/libuaos/uaos_cmd.h`, `uaos_template.h`, and `uaos_syscall.h`. New VFS syscalls (`SYSCALL_MKDIR` through `SYSCALL_GETMOUNTNAME`, 0x20–0x2C) were added to support them. `avail` was likewise migrated off its hardcoded native stub to a userspace binary that queries real memory statistics via `SYSCALL_MEMINFO` (0x2D); the kernel `C:mem` command now uses the same `Mem_GetInfo()` helper.

## Command Reference

The following native C: commands are still implemented in `kernel/shell/`:

| Category | Commands |
|---|---|
| **Volume / Disk** | `info`, `disks`, `diskchange`, `mount`, `format`, `fdisk`, `addbuffers`, `relabel`, `install` |
| **System** | `version`, `mem`, `status`, `info`, `libs`, `ps`, `jobs`, `wait`, `changetaskpri`, `stack`, `why`, `failat`, `quit`, `endcli`, `newcli`, `execute`, `resident`, `strace` |
| **Network** | `ifconfig`, `route`, `ping`, `nslookup`, `ntpd`, `netstart`, `netstop`, `netinfo` |
| **Desktop / Windows** | `loadwb`, `calc`, `clock`, `pointer`, `vim`, `ed`, `guide`, `requestchoice`, `requestfile` |
| **Preferences** | `screenmode`, `font`, `icontrol`, `input`, `palette`, `wbpattern`, `serial`, `printer`, `time`, `locale` |
| **Tools & Commodities** | `exchange`, `blanker` |
| **Printing & CrossDOS** | `print`, `crossdos` |
| **Editors & Help** | `vim`, `ed`, `guide` |
| **Utilities** | `date`, `ask`, `which`, `getenv`, `unset`, `clear`, `reboot` |

The following commands are now on-disk x86-64 ELF64 userspace binaries in `system/userspace/`:

| Category | Commands |
|---|---|
| **Filesystem** | `dir`, `list`, `copy`, `delete`, `rename`, `makedir`, `type`, `more`, `protect`, `attr`, `filenote`, `search`, `sort`, `join`, `grep` |
| **Utilities** | `echo`, `pwd`, `find`, `file`, `strings`, `avail` |

For full syntax and examples, see the `README.md` and `documentation/Dos_Manual.md` in the repository root.

## File Browser Launching

Double-clicking an icon in the Workbench file browser (`filebrowser.c`) calls `ExecFile_Run()` in `exec_file.c`. The loader reads the first four bytes of the selected file to determine whether it is a UAOS wrapper binary (e.g. `UAOS` for a native command), an Amiga Hunk executable, or an ELF64 binary. Native commands are dispatched through `NativeCmd_Run()`, which constructs a minimal `NativeCmdCtx` and invokes the corresponding `Cmd_` function registered in the native command table.

## Scripting

The shell supports basic scripting via `S:Startup-Sequence` and the `execute` command, allowing for automated system initialization. The `newcli`/`newshell` command accepts an optional `from <script>` argument: `ShellWin_OpenWithScript()` opens a new shell window and synchronously runs the named script in it (resolved relative to the invoking shell's cwd), reusing the same `run_script_text` runner and 4 KB script buffer pool as `execute` and the boot `Startup-Sequence`.
