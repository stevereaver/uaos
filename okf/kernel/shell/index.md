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
- Synchronous child execution tracking (blocking on `SIGF_CHILD` when running foreground tasks). `Task_ClearSig(SIGF_CHILD)` is called immediately before `Wait()` in both `inst_exec_uaos_bin()` and `sys_wait()` (`syscall_dispatch.c`) so a stale child-exit signal can't make `Wait()` return early — that race sent redirected output to the shell window instead of the target file.  Foreground waits also mask in `SIGF_BREAKF` so Ctrl-C wakes the wait early and forwards the break to the child task (AmigaOS semantics: programs poll `CheckSignal(SIGBREAKF_CTRL_C)`).
- Break/interrupt handling (UAOS-50): Ctrl-C (raw `0x03`, or Telnet `IAC IP`/`IAC AO` on remote sessions) sets a per-instance `break_req` flag and signals `SIGF_BREAKF`.  The flag is one-shot — consumed by `shell_take_break` (which also flushes the input ring) at `inst_dispatch` entry, in the script block runner, in both `FOR` loops and inside `read_key`/`read_line`; consumers abort with `***Break` instead of executing.  The foreground child wait forwards `SIGF_BREAKF` to the child task **without** consuming the flag, and a `dispatch_broken` marker lets a consumed break propagate to enclosing loops/scripts so `for i=1 to N do cmd` really stops.  At an idle prompt Ctrl-C cancels the current input line (`^C` + fresh prompt) rather than recalling history — this required moving the cursor virtual-key codes (`KBD_VKEY_*` in `ps2kbd.h`, aliased `SHELL_VKEY_*` in `shell_win.h`) out of the ASCII control range, since `SHELL_VKEY_UP` used to be `0x03` itself.  Long-running commands see the break through the `NativeCmdCtx.break_pending` callback (`wait`, `ping`, `more` honour it).

## Command Execution

Commands in UAOS can be:
- **Internal**: Built into the shell (e.g., `alias`, `set`, `cd`, `path`, `prompt`, `failat`, `why`).
- **Resident**: Compiled into the kernel but executed as separate logic (e.g., `mem`, `libs`, `version`).
- **External**: Loaded from disk. This includes:
  - **M68k Amiga Hunk binaries** wrapped with a custom 32-byte UAOS header and executed via the CPU emulator.
  - **Native x86-64 ELF64 binaries** compiled using the `-nostdlib` flag and executed as ring-0 tasks (for VirtualBox NEM compatibility). These use the `INT 0x80` syscall ABI to interact with the kernel.
- **Native C: commands**: Executed in-place by the kernel command dispatcher (`cmd_*.c` in `kernel/shell/`).

### Path resolution

Shell paths follow AmigaDOS conventions (`NAME:` absolute, `:` volume root, leading `/` = one level up per slash). `make_abs_path` (in `kernel/display/shell_win.c` for shell builtins, mirrored in `kernel/shell/cmd_internal.h` for native C: commands and `kernel/exec/syscall_dispatch.c` for userspace syscall paths) additionally normalizes `.` and `..` components via `resolve_dot_components`/`cmd_resolve_dots`, so Unix-style spellings work everywhere: `cd ..`, `cd ../..`, `cd ../foo`, `dir ..`, `copy .. dest`. A `..` at the volume root is dropped — the `NAME:` prefix is the traversal floor.

> [!NOTE]
> As of Phase 7, the following DOS commands have been migrated from kernel-resident native C: stubs to on-disk x86-64 ELF64 userspace binaries: `echo`, `type`, `dir`, `list`, `makedir`, `delete`, `rename`, `copy`, `protect`, `attr`, `grep`, `sort`, `join`, `search`, `filenote`, `more`. These binaries live in `system/userspace/` and use the shared helpers in `system/libuaos/uaos_cmd.h`, `uaos_template.h`, and `uaos_syscall.h`. New VFS syscalls (`SYSCALL_MKDIR` through `SYSCALL_GETMOUNTNAME`, 0x20–0x2C) were added to support them. The native `dir` backend allocates its 256-entry `VfsDirEnt` workspace per recursion level from `ELF64_HeapAlloc` — a shared static array was clobbered by the `ALL` recursion (UAOS-221), and an ~11 KB stack array would risk the 32 KB command stack. `avail` was likewise migrated off its hardcoded native stub to a userspace binary that queries real memory statistics via `SYSCALL_MEMINFO` (0x2D); the kernel `C:mem` command now uses the same `Mem_GetInfo()` helper.

### Return Codes (`last_rc` / `prev_rc`)

Each command dispatched through `run_cmd()` starts with `last_rc = 0` —
a command that does not call `set_rc` counts as success (AmigaDOS
convention).  Before the reset, the previous value is copied into
`prev_rc`, which is what `WHY` (`ctx->get_prev_rc`) and the `IF
WARN`/`ERROR`/`FAIL` script conditions inspect.  `check_failat()`
therefore only prints `FAILAT: return code N >= threshold M` when the
command that just ran actually failed — previously a stale nonzero
`last_rc` survived across successful commands and the warning repeated
after every command until the shell was closed.  `rx` still reads the
just-dispatched command's rc through `get_last_rc` for propagation.

## Command Reference

The following native C: commands are still implemented in `kernel/shell/`:

| Category | Commands |
|---|---|
| **Volume / Disk** | `info`, `disks`, `diskchange`, `mount`, `format`, `fdisk`, `fsck`, `addbuffers`, `relabel`, `install` |
| **System** | `version`, `mem`, `status`, `info`, `libs`, `ps`, `jobs`, `wait`, `changetaskpri`, `stack`, `why`, `failat`, `quit`, `endcli`, `newcli`, `execute`, `resident`, `resload`, `run`, `runback`, `strace`, `rx`, `klog` (`debug`), `dmesg`, `irqstat`, `usbdiag`, `chiptrace`, `memcheck`, `crash` |
| **Diagnostics** (UAOS-188) | `taskdump`, `taskstat`, `watchdog`, `ports`, `timers`, `handles`, `netstat`, `diskdiag`, `pciscan`, `irqroute`, `peek`, `poke`, `irqaudit`, `sercon`, `tickcheck`, `etrace`, `prof`, `failalloc`, `pktmon` — see [Diagnostics toolkit](/kernel/diag/index.md) |
| **Network** | `ifconfig`, `route`, `ping`, `nslookup`, `ntpd`, `netstart`, `netstop`, `netinfo` |
| **Desktop / Windows** | `loadwb`, `calc`, `clock`, `pointer`, `vim`, `ed`, `guide`, `requestchoice`, `requestfile` |
| **Preferences** | `screenmode`, `font`, `icontrol`, `input`, `palette`, `wbpattern`, `serial`, `printer`, `time`, `locale` |
| **Tools & Commodities** | `exchange`, `blanker` |
| **Printing & CrossDOS** | `print`, `crossdos` |
| **Editors & Help** | `vim`, `ed`, `guide` |
| **Utilities** | `date`, `ask`, `which`, `getenv`, `unset`, `clear`, `reboot` |
| **Shell-state wrappers** | `alias`, `unalias`, `path` (forward to the shell built-ins via `dispatch_line`); `skip`, `lab` (script keywords — native entries exist so `C:skip`/`C:lab` resolve; `skip` prints a note, `lab` is a no-op) |

`format` reports per-stage FAT32 errors (e.g. "FAT32: failed to write FSINFO") instead of a bare "Format failed."; the same return-code switch lives in `inst_cmd_format` (`shell_win.c`) and `Cmd_Format` (`cmd_format.c`). Syntax is `format <device> [filesystem] [NOICON]` — like AmigaOS Format, it creates a `Trashcan` drawer + `Trashcan.info` on the fresh volume by default; `NOICON` skips it (UAOS-36).

`run` (UAOS-75) has AmigaOS semantics: it re-dispatches its argument line through the shell's background-job queue (`dispatch_line` + trailing `&`), so the child resolves through the normal `run_cmd` chain (built-ins → resident → native C: → PATH/cwd binaries, NATIVE/M68K/X64 headers).  It no longer consults only the ROM embedded-binary registry (`UAOS_Emu_RunByName`), which is kept solely as a fallback when no shell dispatch context exists.  If `run`'s own stdout was redirected, the spec is propagated to the detached child via `NativeCmdCtx.out_redirect` (filled from `g_redir.path` in `shell_make_ctx`), so `run >NIL: C:ntpd` silences the child too.  Because M68K/X64 payloads become real `UaosTask`s, `inst_exec_uaos_bin` skips the foreground `Wait(SIGF_CHILD)` while `g_bg_running` is set — otherwise a detached long-running binary would block the job pump, which lives in the main UI loop.

Background-job completion tracking (UAOS-116): the job pump (`bg_run_next` in `shell_win.c`, driven by `ShellWin_PollJobs` from the idle task) no longer marks a job `done` when dispatch returns — binary commands spawn a `UaosTask` and return immediately, which previously printed `[n] done` instantly and left `jobs` empty while the task still ran.  While a job is being dispatched the global `g_task_bg_job` holds its job number; `Task_CreateNative`/`Task_CreateX64` stamp it into the new task's `bg_job` field (`Task_CreateM68k` inherits via `Task_CreateNative`).  The pump keeps the job `active` while any task carries its number, and a reaper pass at the top of each poll prints `[n] done` and retires the job once the last stamped task reaches `TASK_REMOVED` — `bg_job_alive` matches on the job number rather than a task pointer because `Task_CreateX64` recycles `TASK_REMOVED` slots.  Synchronous commands (builtins, scripts, native C: commands) spawn no task and still complete immediately after dispatch.

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
  - `/M` — multiple values (`<argname>` expands to all values joined by spaces). Positional `/M` matching reserves enough remaining tokens to satisfy later positional items — `FROM/M,TO/A` with `copy a b` assigns `FROM=a`, `TO=b` rather than letting `/M` swallow `b`. The same rule is implemented in the userspace template parser `system/libuaos/uaos_template.h` used by on-disk commands like `C:copy`.
  - `/F` — free-form (absorbs all remaining tokens)
  
  When a `.key` template is present, `$1`..`$9` are set in **template-item order** (the order names appear in the `.key` line), not raw token order. This ensures `<argname>` resolves correctly even when `/K` keyword args are passed out of order. If template matching fails, `execute` prints a warning and falls back to raw positional assignment. When there is no `.key` declaration, `$1`..`$9` remain raw positional tokens (backward compatible). `$*` always holds the full raw argument string.

  The parse/match/bind step lives in the `exec_bind_template()` helper so its `CmdTemplateResult` leaves the stack before `run_script` runs — holding it across nested script dispatch previously kept ~19.5 KB live on the Shell task's 32 KB stack (UAOS-228).

- **`shell_win.c` (script runner)**: `run_script_text()` pre-scans for the first `.key` line and populates a per-nest-level key map (`g_script_keys[]`) via `script_parse_keys()`. At most 16 names are recorded per script; nested `execute` scripts get their own key map. `expand_vars()` then resolves `<argname>` references by looking up the name in the active key map, mapping it to its positional index, and reading the corresponding `$n` variable. `<argname>` only fires when the name matches a declared key and is terminated by `>`; otherwise `<` is emitted literally so I/O redirection (`< file`) still works. Outside a script (no active key map), `<...>` is never consumed.

### Backtick Command Substitution

`` `command` `` runs `command` and splices its captured stdout in place, with a single trailing newline stripped. This is handled in `expand_vars()` via `run_backtick()`, which:

1. Creates a unique temp file `T:bt<N>`,
2. Saves the current `g_redir`/`g_capture_mode` state, routes command output to the temp file, and sets `g_capture_mode = 1` to suppress the prompt echo inside `inst_dispatch()`,
3. Dispatches the command normally (so `$var` expansion, pipes, and nested backticks all work),
4. Restores the saved redirect state, reads the temp file back, strips trailing CR/LF, and deletes it.

Backtick substitution applies everywhere `expand_vars()` runs — command lines, `echo` arguments, and `IF` condition strings — but not in the prompt string (`expand_prompt()` is separate).

### Quote Stripping & Quoted Tokens

Both template tokenizers (`tokenise()` in `cmd_template.c` and `uaos_tmpl_tokenise()` in `uaos_template.h`) treat `"..."` as a single token that may contain spaces, strip the quotes, and mark the token `quoted` so it is exempt from keyword matching (UAOS-110). This makes the documented AmigaDOS escape work — `echo "FOR x=1 TO 3"` prints literally instead of `TO` binding `3` as Echo's `TO/K` destination. A quote pair that happens to fill a whole whitespace-delimited word is still stripped and marked quoted. The `SET` built-in (`inst_cmd_set` in `shell_win.c`) strips one layer of surrounding double-quotes as before.

### Keyword Binding

Matching AmigaDOS `ReadArgs` semantics, **every** template item name acts as a keyword — `/K` merely makes the keyword *required*. `search FROM RAM: SEARCH plain` now binds `FROM={RAM:}`, `SEARCH=plain` (previously the literal `FROM`/`SEARCH` tokens were absorbed positionally into `FROM/M`, so `uaos_opendir` was called on `cwd/FROM` — UAOS-112). Quoted tokens never match a keyword. As on real AmigaDOS, a filename that collides with a template item name must be quoted to be taken positionally.

### `CmdTemplateResult` stack footprint (UAOS-228)

`CmdTemplateResult` lives on the caller's stack — `NativeCmd_Run` keeps one live across the whole command handler via `ctx->template`, and `exec_bind_template` holds one while binding script `$n` vars. The kernel parser (`kernel/shell/cmd_template.{h,c}`) therefore keeps `/M` multi-values in a shared `multi_pool[2][8][128]` inside the result: each `/M` template item claims one 8-slot block at parse time (`value_index`, capped by `CMD_MAX_MULT_ITEMS`; a third `/M` item is a parse error). This shrank `CmdTemplateItem` from ~1.2 KB (embedded `values[8][128]`) to ~160 B and `CmdTemplateResult` from ~19.5 KB to ~5 KB — plus ~4.4 KB of `TokArray` in `CmdTemplate_MatchArgs` — taking the Shell task's worst-case peak from ~31.4 KB (96 %, near-overflow) to ~2.5 KB for ordinary commands and ~25.9 KB through nested `execute` (the residual is the X64-launch/dispatch path, not the template). `CmdTemplate_GetMulti()` reads pool slots; the accessor API is unchanged.

The userspace parser (`system/libuaos/uaos_template.h`) got the same shared-pool treatment under UAOS-256 — its per-item `values[4][128]` had silently dropped every `/M` value past the fourth, so `C:echo` (template `STRING/M,...`) printed only the first four words of a line and `copy`/`type`/`join`/`delete`/`list`/`search` lost file arguments beyond four. `UaosTmpl` now carries `multi_pool[2][32][128]` (slots match `UAOS_TMPL_MAX_TOKENS`), giving `UaosTmpl` ~11.5 KB — comfortable on the 256 KB x64 task stack. Accessor API (`uaos_tmpl_count`/`uaos_tmpl_multi`) unchanged.

### Single-line IF / FOR at the prompt

`inst_dispatch()` skips its global `expand_vars()` pass for lines beginning with `if` or `for`: the FOR body would otherwise expand before the loop variable is bound, making `for i=1 to 3 do echo $i` print empty (UAOS-110). `run_cmd` scans the header token-wise for `do`, expands only the header (`var=start TO end [STEP k]`), and re-dispatches the raw body per iteration. `script_find_then`/`script_eval_cond_n` locate `THEN` by scanning tokens rather than skipping one word, so multi-token conditions (`if 1 eq 1 then ...`, `if exists f then ...`) work at the prompt and inside scripts, and the condition is evaluated on the bounded text before `THEN` — previously `EXISTS` swallowed the `THEN ...` tail into the tested path.

### `rx` Command (`kernel/shell/cmd_rx.c`)

The native `rx` command wraps the Regina Rexx interpreter (`REXX:rexx`) to provide ARexx-compatible scripting. It supports two forms:

- **Inline program**: `rx "say 'Hello'"` — writes the quoted string to `T:rx_temp.rexx`, dispatches `REXX:rexx T:rx_temp.rexx`, then cleans up.
- **File-based program**: `rx myscript arg1` — dispatches `REXX:rexx <filename> [args]`. Bare names (no path/extension) are searched in `REXX:` with `.rexx` appended automatically.

The return code from the dispatched `rexx` command is propagated back through the shell's `get_last_rc`/`set_rc` callbacks.
