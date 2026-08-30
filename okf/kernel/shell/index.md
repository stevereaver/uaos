---
type: Kernel Subsystem
title: UAOS Shell
description: The command-line interface for UAOS.
resource: /kernel/shell/
tags: [shell, cli, commands, scripting, template, backtick]
timestamp: 2026-08-30T00:00:00Z
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
| **System** | `version`, `mem`, `status`, `info`, `libs`, `ps`, `jobs`, `wait`, `changetaskpri`, `stack`, `why`, `failat`, `quit`, `endcli`, `newcli`, `execute`, `resident`, `strace`, `rx` |
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

### Script Template Arguments (`.key` / `<argname>`)

AmigaDOS-style script template arguments are implemented across two files:

- **`C:execute` (`kernel/shell/cmd_execute.c`)**: After loading the script, `Cmd_Execute()` scans for a `.key` declaration via `exec_find_key()`. If found, the template spec is parsed with `CmdTemplate_Parse()` and the raw argument string is matched with `CmdTemplate_MatchArgs()`. This honours all AmigaDOS qualifiers:
  - `/A` — required (missing args produce a warning)
  - `/K` — keyword args, passed as `name=value` or `name value` (order-independent)
  - `/S` — switch (`<argname>` expands to `1` if present, empty if absent)
  - `/N` — numeric
  - `/M` — multiple values (`<argname>` expands to all values joined by spaces)
  - `/F` — free-form (absorbs all remaining tokens)
  
  When a `.key` template is present, `$1`..`$9` are set in **template-item order** (the order names appear in the `.key` line), not raw token order. This ensures `<argname>` resolves correctly even when `/K` keyword args are passed out of order. If template matching fails, `execute` prints a warning and falls back to raw positional assignment. When there is no `.key` declaration, `$1`..`$9` remain raw positional tokens (backward compatible). `$*` always holds the full raw argument string.

- **`shell_win.c` (script runner)**: `run_script_text()` pre-scans for the first `.key` line and populates a per-nest-level key map (`g_script_keys[]`) via `script_parse_keys()`. At most 16 names are recorded per script; nested `execute` scripts get their own key map. `expand_vars()` then resolves `<argname>` references by looking up the name in the active key map, mapping it to its positional index, and reading the corresponding `$n` variable. `<argname>` only fires when the name matches a declared key and is terminated by `>`; otherwise `<` is emitted literally so I/O redirection (`< file`) still works. Outside a script (no active key map), `<...>` is never consumed.

### Backtick Command Substitution

`` `command` `` runs `command` and splices its captured stdout in place, with a single trailing newline stripped. This is handled in `expand_vars()` via `run_backtick()`, which:

1. Creates a unique temp file `T:bt<N>`,
2. Saves the current `g_redir`/`g_capture_mode` state, routes command output to the temp file, and sets `g_capture_mode = 1` to suppress the prompt echo inside `inst_dispatch()`,
3. Dispatches the command normally (so `$var` expansion, pipes, and nested backticks all work),
4. Restores the saved redirect state, reads the temp file back, strips trailing CR/LF, and deletes it.

Backtick substitution applies everywhere `expand_vars()` runs — command lines, `echo` arguments, and `IF` condition strings — but not in the prompt string (`expand_prompt()` is separate).

### Quote Stripping

The template tokenizer (`tokenise()` in `cmd_template.c`) and the `SET` built-in (`inst_cmd_set` in `shell_win.c`) strip one layer of surrounding double-quotes from argument values. This matches AmigaDOS conventions where `SET foo "bar"` stores `bar` (not `"bar"`) and `copy T:file ""` uses the empty string (current directory). Only tokens where the first AND last character are both `"` are stripped; partial quotes inside a token are preserved.

### `rx` Command (`kernel/shell/cmd_rx.c`)

The native `rx` command wraps the Regina Rexx interpreter (`REXX:rexx`) to provide ARexx-compatible scripting. It supports two forms:

- **Inline program**: `rx "say 'Hello'"` — writes the quoted string to `T:rx_temp.rexx`, dispatches `REXX:rexx T:rx_temp.rexx`, then cleans up.
- **File-based program**: `rx myscript arg1` — dispatches `REXX:rexx <filename> [args]`. Bare names (no path/extension) are searched in `REXX:` with `.rexx` appended automatically.

The return code from the dispatched `rexx` command is propagated back through the shell's `get_last_rc`/`set_rc` callbacks.
