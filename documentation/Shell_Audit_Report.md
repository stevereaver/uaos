# UAOS Shell / AmigaDOS Compatibility Audit

**Scope:** Shell built-ins, C: native commands, scripting, environment variables, assigns, and I/O redirection as documented in `documentation/Dos_Manual.md`.

**Method:** Static source review of `kernel/shell/`, `kernel/dos/`, `kernel/display/shell_win.c`, `system/userspace/`, and `system/Startup-Sequence`. No runtime testing was performed because the OS requires a full ISO build + emulator or hardware boot.

**Overall result:** The shell is broadly implemented and matches the manual for the common built-ins, native C: registry, VFS assigns, template parsing, and redirection. Several commands are present but simplified or only partially implemented, and a few documented behaviors are missing or incorrect.

---

## 1. Conformant Items

| Feature | Status | Evidence |
|---------|--------|----------|
| **Built-ins** `help`, `cd`, `alias`, `unalias`, `set`, `unset`, `path`, `setenv`, `unsetenv`, `showconfig` | Implemented | `kernel/display/shell_win.c` lines 1434-1932 (command handlers) and lines 4009-4032 (built-in dispatch table). |
| **Case-insensitive command matching** | Implemented | `kernel/display/shell_win.c` `seq_ci()` at line 413 and `cmd_match()` at line 421. |
| **Command history (Up/Down)** | Implemented | `kernel/display/shell_win.c` history buffer and `ShellWin_HandleKey()` at line 1236. |
| **Tab auto-complete** | Implemented | `kernel/display/shell_win.c` `tab_complete()` at line 4397; completes both built-in/native names and VFS paths. |
| **Command templates (`/A /K /S /N /M /F`)** | Implemented | `kernel/shell/cmd_template.h` (kernel) and `system/libuaos/uaos_template.h` (userspace). Both parse all documented qualifiers. |
| **Native command registry (`C:`)** | Implemented | `kernel/shell/native_cmd.c` `g_native_cmds[]` at line 32; `NativeCmd_Run()` dispatches by name. |
| **Native commands present** `version`, `mem`, `libs`, `clear`, `reboot`, `info`, `date`, `which`, `disks`, `fdisk`, `format`, `pointer`, `run`, `assign`, `execute`, `loadwb`, `ifconfig`, `ping`, `route`, `nslookup`, `ntpd`, `netstart`, `netstop`, `newcli`, `resident`, `relabel`, `mount`, `ask`, `ps`, `jobs`, `wait`, `prompt`, `why`, `failat`, `quit`, `endcli`, `status`, `avail`, `stack`, `getenv`, `unset`, `install`, `diskchange`, `addbuffers`, `requestchoice`, `requestfile`, `changetaskpri` | Implemented as kernel-native handlers | `kernel/shell/cmd_*.c` files; registered in `kernel/shell/native_cmd.c`. |
| **Userspace C: commands** `echo`, `type`, `dir`, `copy`, `delete`, `rename`, `protect`, `attr`, `pwd`, `grep`, `more`, `find`, `file` | Implemented as x86-64 ELF64 | `system/userspace/*.c`; staged into `SYS_ROOT/C` by `scripts/build_iso.sh` lines 821-893. |
| **Assigns (kernel defaults)** | Implemented | `kernel/dos/vfs.c` `VFS_SetupWorkbenchAssigns()` at line 178 creates `SYS:`, `LIBS:`, `C:`, `S:`, `DEVS:`, `L:`. |
| **Assigns (user-defined, ADD, DEFER)** | Implemented | `kernel/shell/cmd_assign.c` and `kernel/dos/vfs.c` `VFS_AddAssign()` at line 797. |
| **Startup-Sequence** | Implemented and shipped | `system/Startup-Sequence` creates `ENV:`, `T:`, `Clips:`, `REXX:`, `PRINTERS:`, `KEYMAPS:`, `gnu:`, `LOCALE:`/`HELP:` (deferred), extends `LIBS:`, and runs `LoadWB`. |
| **I/O redirection `>`, `>>`, `<`** | Implemented | `kernel/display/shell_win.c` `inst_dispatch()` redirection parsing at line 4723; output captured to VFS files. |
| **Local vs global variables** | Implemented | `set`/`unset` update shell-local arrays; `setenv`/`unsetenv` also write/read `ENV:` files via VFS at `kernel/display/shell_win.c` lines 1594-1688. |
| **IF / ELSE / ENDIF, FOR, EXISTS, EQ/NE/NOT** | Implemented | `kernel/display/shell_win.c` `script_*` functions around lines 3730-3890; `EXISTS`, `EQ`, `NE`, `NOT` operators in `script_eval_cond()`. |
| **Script comments (`;`)** | Implemented | `kernel/display/shell_win.c` `script_*` functions skip lines starting with `;`. |
| **Arithmetic expansion `$[expr]`** | Implemented | `kernel/display/shell_win.c` `expand_vars()` at line 4274. |
| **Break (Ctrl-C) handling** | Implemented | `kernel/display/shell_win.c` checks `g_break_requested` at line 4695. |
| **Networking commands** `ifconfig`, `ping`, `route`, `nslookup`, `ntpd`, `netstart`, `netstop` | Implemented | `kernel/shell/cmd_ifconfig.c`, `cmd_ping.c`, `cmd_route.c`, `cmd_nslookup.c`, `cmd_ntpd.c`, `cmd_netstart.c`, `cmd_netstop.c`. |
| **Desktop GUI launchers** `calculator`, `clock`, `pointer`, `loadwb`, `vim`, `netinfo` | Implemented | `kernel/shell/cmd_calc.c`, `cmd_clock.c`, `cmd_pointer.c`, `cmd_loadwb.c`, `cmd_vim.c`, `cmd_netinfo.c`. |
| **`resident` list/flush/pure/remove** | Implemented | `kernel/shell/cmd_resident.c`; registry in `kernel/shell/resident_cmd.h` with 16 slots (`MAX_RESIDENT_CMDS`). |

---

## 2. Deviations / Partial / Missing

### 2.1 Shell Built-ins

| Issue | Severity | Evidence / Notes |
|-------|----------|------------------|
| **`execute` is not a shell built-in** — the task lists it as a built-in, and `shell_win.c` even contains an unused `inst_cmd_execute()` at line 2260, but it is not registered in the built-in dispatch table at line 4009. The actual `execute` command lives as a `C:` native command (`kernel/shell/cmd_execute.c`). | Minor / Architecture drift | `kernel/display/shell_win.c` line 2260 (dead code), `kernel/shell/native_cmd.c` line 32 (registration). |
| **`rename` is not a shell built-in** — the task lists it as a built-in; `shell_win.c` has a dead `inst_cmd_rename()` at line 1675 that only prints "Rename not yet implemented". The real `rename` command is a userspace `C:` binary (`system/userspace/rename.c`). | Minor / Architecture drift | `kernel/display/shell_win.c` lines 1675-1680, `system/userspace/rename.c`. |
| **Dead built-in stubs** for `version`, `mem`, `libs`, `clear`, `reboot`, `date` exist in `shell_win.c` but are not registered in the built-in table; the shell runs them via the native C: registry instead. They are harmless but confusing. | Minor cleanup issue | `kernel/display/shell_win.c` lines 1936-2020, `native_cmd.c` registration. |

### 2.2 Pre-defined / Startup Variables

| Issue | Severity | Evidence / Notes |
|-------|----------|------------------|
| **`$Workbench` and `$Kickstart` are never set.** The manual says they are "Set during startup (then unset)". `Startup-Sequence` tries `SetEnv Workbench $Workbench` and `SetEnv Kickstart $Kickstart` then `UnSet` them, but no kernel code initializes these local variables before the startup sequence runs. They will expand to empty strings, producing empty `ENV:Workbench` and `ENV:Kickstart` files. | Medium | `system/Startup-Sequence` lines 41-44; grep across `kernel/` and `system/` finds no assignment of `Workbench`/`Kickstart` as shell variables. |

### 2.3 Variable Expansion

| Issue | Severity | Evidence / Notes |
|-------|----------|------------------|
| **`${name}` brace expansion is not implemented.** The manual says "Use `$name` or `${name}`". The implementation only supports `$name` (and `$[expr]`). | Medium | `kernel/display/shell_win.c` `expand_vars()` at line 4246 only reads a plain identifier after `$`. |

### 2.4 I/O Redirection

| Issue | Severity | Evidence / Notes |
|-------|----------|------------------|
| **`>NIL:` is treated as a literal VFS path**, not a special null device. The manual itself notes this fallback ("treated as literal paths unless NIL: is explicitly mounted"), so this is documented behavior but still a limitation. | Low / Documented limitation | `kernel/display/shell_win.c` line 4723-4750; no special-case for `NIL:`. |
| **No pipe support (`|`)** — not explicitly requested by the manual, but worth noting. | Low | Not implemented in `inst_dispatch()`. |

### 2.5 C: Native / Userspace Commands

| Command | Status | Evidence / Notes |
|---------|--------|------------------|
| **`info [device]`** — partially implemented. Only `RAM:` has special handling; other devices are looked up via `BlockDev_Find()`, not VFS volume mounts. Logical volumes mounted via `VFS_MountPartition()` (e.g. `Workbench:`) may not be found. | Partial | `kernel/shell/cmd_info.c`. |
| **`disks`** — only checks/hardcodes `virtio0`; does not enumerate all block devices as the manual states. | Partial | `kernel/shell/cmd_disks.c`. |
| **`copy`** — `DATES` switch is accepted but ignored; timestamps are not preserved. Wildcard copy is supported. | Partial | `system/userspace/copy.c` lines 98-113. |
| **`delete`** — works for files and recursive dirs with `ALL`; `FORCE` clears the delete protection bit and retries. No `QUIET` summary suppression issues. | Mostly OK | `system/userspace/delete.c`. |
| **`rename`** — only single-file move/rename; no wildcard/pattern support. | Partial | `system/userspace/rename.c`. |
| **`protect`** — supports `+/-` syntax and `FLAGS`/`ADD`/`SUB` keywords, which is more than the manual. Wildcard/`ALL` supported. | OK / Extra features | `system/userspace/protect.c`. |
| **`attr`** — shows protection bits and basic attributes; no extended comments or AmigaDOS `Comment` display. | Partial | `system/userspace/attr.c`. |
| **`dir`** — implements `ALL`, `DATES`, `INTER`, `KEYS`, `DIRS`, `FILES`, `OPT A|D`. Pattern matching works. | Mostly OK | `system/userspace/dir.c`. |
| **`echo`** — implements `NOLINE`, `FIRST`, `LEN`, `TO`. | OK | `system/userspace/echo.c`. |
| **`type`** — implements `TO`, `HEX`, `NUMBER`, `OPT H|N`. | OK | `system/userspace/type.c`. |
| **`grep`** — simple substring search, not regular expressions; `CI` and legacy `-i` supported. | Partial | `system/userspace/grep.c`. |
| **`more`** — hardcodes 20 lines per page; does not query actual terminal height. | Partial | `system/userspace/more.c`. |
| **`run`** — documented as "execute an embedded Amiga M68k binary from the ROM registry"; implementation calls `UAOS_Emu_RunByName()` which does exactly that. | OK | `kernel/shell/cmd_run.c`. |
| **`newcli [from <script>]`** — `from` argument is ignored. | Partial | `kernel/shell/cmd_newcli.c` line 12. |
| **`wait`** — only supports seconds; time-of-day form not implemented. | Partial | `kernel/shell/cmd_wait.c`. |
| **`avail`** — prints hardcoded fake values (512 MB RAM, etc.) rather than querying the memory manager. | Partial / Misleading | `kernel/shell/cmd_avail.c` lines 11-16. |
| **`stack`** — read-only; dynamic resizing not implemented. | Partial | `kernel/shell/cmd_stack.c`. |
| **`requestchoice` / `requestfile`** — fall back to text prompts in the shell because Intuition modal requesters are not available. They set `$RC`/`$RESULT` correctly. | Partial | `kernel/shell/cmd_requestchoice.c`, `cmd_requestfile.c`. |
| **`showconfig`** — built-in prints a fixed list of detected hardware strings; does not actually enumerate PCI/ACPI/devices. | Partial | `kernel/display/shell_win.c` `inst_cmd_showconfig()`. |
| **`date`** — proper implementation is the native C: command (`cmd_date.c`); the `shell_win.c` built-in stub is dead. | OK / Dead stub | `kernel/shell/cmd_date.c`, `kernel/display/shell_win.c` `inst_cmd_date()`. |
| **`resident`** cache size is 16 slots × 64 KB (up to 1 MB), not the 256 KB total stated by the manual. | Minor | `kernel/shell/resident_cmd.h` line 18-20. |
| **`mount`** — loads handler from `L:` and registers in `DosList`. The `from` keyword and auto-derived handler names are supported. | OK | `kernel/shell/cmd_mount.c`. |
| **`install`, `diskchange`, `addbuffers`** — Implemented as approximations; `addbuffers` only tracks a counter, `install` writes a 1024-byte bootblock stub. | Partial | `kernel/shell/cmd_install.c`, `cmd_diskchange.c`, `cmd_addbuffers.c`. |
| **`which`** — checks built-ins, native registry, `PATH`, and current directory. | OK | `kernel/shell/cmd_which.c`. |

### 2.6 Scripting

| Feature | Status | Evidence / Notes |
|---------|--------|------------------|
| **FOR loop** | Implemented | `kernel/display/shell_win.c` `script_for_line()` pattern matching. |
| **`IF` with `EXISTS`, `EQ`, `NE`, `NOT`** | Implemented | `kernel/display/shell_win.c` `script_eval_cond()`. |
| **Nested scripts / `execute` args `$1`..`$9` and `$*`** | Implemented | `kernel/shell/cmd_execute.c` lines 84-107. |
| **Return codes / `failat` / `why`** | Implemented | `kernel/display/shell_win.c` tracks last RC; `cmd_failat.c`, `cmd_why.c`. |

---

## 3. Untested / Cannot Test

The following were reviewed statically only; runtime verification would require building the ISO and booting it in QEMU/VirtualBox or on real hardware:

- Actual command execution and output formatting of all C: binaries.
- Networking stack (`ifconfig`, `ping`, `route`, `nslookup`, `ntpd`, `netstart`, `netstop`) on live hardware or emulated NIC.
- Disk partitioning/formatting (`fdisk`, `format`, `install`, `diskchange`, `addbuffers`) on real or emulated block devices.
- Workbench desktop launch (`LoadWB`, `calculator`, `clock`, `pointer`, `vim`, `netinfo`) and GUI rendering.
- M68k binary execution via `run` and the emulator layer.
- Resident command loading/flush behavior under memory pressure.
- Full `Startup-Sequence` boot path including `User-Startup` execution.

No automated unit tests were found for the shell or VFS layer, so no test suite was run.

---

## 4. Recommended Fixes

1. **Set `$Workbench` and `$Kickstart` before running `Startup-Sequence`.** In `kernel/display/shell_win.c` or in the startup caller, pre-populate these local variables (e.g. `Workbench=Workbench:` and `Kickstart=47.1` or appropriate version) so the documented `SetEnv Workbench $Workbench`/`UnSet` sequence works as intended.
2. **Add `${name}` variable expansion** in `expand_vars()` (`kernel/display/shell_win.c` line 4246).
3. **Remove or register the dead built-in stubs** for `execute`, `rename`, `version`, `mem`, `libs`, `clear`, `reboot`, and `date` to eliminate confusion. If the architecture intends them to be C: commands, delete the unused `inst_cmd_*` functions.
4. **Fix `disks`** to enumerate `BlockDev_GetList()` instead of hardcoding `virtio0`.
5. **Fix `info`** to query VFS-mounted volumes (`VFS_FindVol` / `VFS_ListAssigns`) in addition to block devices, so `info Workbench:` and other logical volumes work.
6. **Implement `copy DATES`** by copying the source modification time to the destination.
7. **Make `avail` query real memory statistics** or change the command description to note that values are placeholders.
8. **Wire `newcli from <script>`** so the optional startup script is executed in the new shell.
9. **Document `$[expr]` arithmetic expansion** in the manual, or decide whether it should remain undocumented.
10. **Add a true `NIL:` device or VFS sink** so `>NIL:` discards output instead of creating a file.

---

## 5. Summary of Investigation Activities

- No source files were modified during this audit.
- Files read (key paths):
  - `documentation/Dos_Manual.md`
  - `kernel/display/shell_win.c`
  - `kernel/shell/native_cmd.c`
  - `kernel/shell/native_cmd.h`
  - `kernel/shell/cmd_*.c` (all native command implementations)
  - `kernel/dos/vfs.c`
  - `kernel/shell/resident_cmd.h`
  - `system/userspace/*.c`
  - `system/Startup-Sequence`
  - `system/S/User-Startup`
  - `scripts/build_iso.sh`
- Commands run: `find`, `grep`, `ls` to locate files; file reads via the read tool.
- No build or runtime tests were executed (not feasible without a full ISO build and emulator boot).
