---
type: System Layout
title: UAOS System Layout
description: Overview of the Amiga-style system directory structure in UAOS, including the GNU coreutils layer.
tags: [layout, filesystem, workbench, gnu, coreutils]
timestamp: 2026-07-01T00:30:00Z
---

# UAOS System Layout

UAOS follows the classic AmigaOS directory structure to maintain compatibility and familiarity for Amiga users.

## Core Directories

- **`C:` (Commands)**: Shell commands and utilities (e.g., `dir`, `copy`, `format`).
- **`DEVS:` (Devices)**: Device drivers and configuration files.
- **`L:` (Loadable Handlers)**: Filesystem and device handlers (e.g., `FastFileSystem`, `aux-handler`).
- **`LIBS:` (Libraries)**: Shared libraries.
- **`S:` (Scripts)**: System scripts, including `Startup-Sequence`.
- **`SYS:` (System Utilities)**: Core system tools and the Workbench itself.
- **`Demos:` (Demonstrations)**: Example M68k programs shipped with the system, such as `CopperBars`, which exercises the custom chipset and Amiga libraries.

## Special Assigns

UAOS uses "Assigns" to create logical device names:
- **`Workbench:`**: The main boot volume.
- **`T:`**: Temporary directory (often in RAM:).
- **`ENV:`**: Environment variables.
- **`CLIPS:`**: Clipboard data.
- **`gnu:`**: POSIX-style directory tree providing GNU core utilities (`gnu/usr/bin`, `gnu/bin`, `gnu/usr/local/bin`). Created by `S:Startup-Sequence` and mapped to `Workbench:gnu`.
- **`REXX:`**: Regina Rexx interpreter and scripts, mapped to `SYS:REXX`.
- **`ACE:`**: ACE Basic compiler installation, mapped to `SYS:ACE`. Sub-assigns: `ACElib:`, `ACEbmaps:`, `ACEinclude:`, `ACEsubmods:`.

## Userspace Library (`system/libuaos/`)

Native Ring-0 userspace programs are built against a minimal freestanding library in `system/libuaos/`:

- **`uaos_start.c`**: Entry point (`_start`) that parses arguments and calls `main`.
- **`uaos_libc.h`**: Header-only static-inline implementations of `strlen`, `strcmp`, `strcpy`, `memcpy`, `memset`, `strlcat`, and character helpers.
- **`uaos_syscall.h`**: Inline wrappers for every `INT 0x80` syscall and the `uaos_gui_event`, `uaos_dirent`, `uaos_stat`, and `uaos_meminfo` structures. Extended in Phase 7 with VFS mutation/metadata syscalls (0x20–0x2C): `mkdir`, `delete`, `rename`, `setprotection`, `getprotection`, `getcomment`, `setcomment`, `getvolumeinfo`, `readkey`, `getattrs`, `setattrs`, `getmountcount`, `getmountname`. `SYSCALL_MEMINFO` (0x2D) exposes the kernel memory query API (`uaos_meminfo()`), used by `C:avail`. Also defines AmigaDOS `FIBF_*` protection bit constants and `UAOS_ATTR_*` flags.
- **`uaos_template.h`**: Header-only AmigaDOS-style command template parser (ported from `kernel/shell/cmd_template.c`). Supports `/A`, `/K`, `/S`, `/N`, `/M`, `/F` qualifiers with `uaos_tmpl_parse()`, `uaos_tmpl_match()`, and query helpers (`uaos_tmpl_switch()`, `uaos_tmpl_string()`, `uaos_tmpl_int()`, `uaos_tmpl_count()`, `uaos_tmpl_multi()`).
- **`uaos_cmd.h`**: Shared helpers for userspace C: command binaries — output (`put_s`, `put_c`, `put_line`), argument reconstruction (`cmd_build_args`), path resolution (`cmd_make_abs`, `cmd_join_path`, `cmd_split_path_pat`), AmigaDOS pattern matching (`cmd_pattern_match` with `#?`, `?`, `*`, `%` wildcards), keyword helpers (`cmd_kw_find`, `cmd_kw_strip`), numeric formatting, and date formatting (`cmd_fmt_mtime`).
- **`uaos_getopt.h`**: Freestanding GNU-style `getopt_long` parser for the `gnu:` coreutils layer. Supports short options, long options (`--name`, `--name=value`, `--name value`), `no_argument`/`required_argument`/`optional_argument` modes, automatic `--` terminator handling, and the `UAOS_GO_LONG + N` sentinel for long-only options. Exposes `uaos_getopt_long()`, `uaos_operands_count()`, `uaos_operand()`, `uaos_optarg_long()`, and the global `g_optarg`/`g_optind`.
- **`uaos_hash.h`**: Freestanding cryptographic hash implementations for the `*sum` coreutils. Provides `md5_ctx`/`sha1_ctx`/`sha256_ctx`/`sha512_ctx`/`blake2b_ctx` with `*_init`/`*_update`/`*_final` interfaces, a POSIX `crc32_update()` for `cksum`, and a `hash_to_hex()` helper for hex digest output.

## Native Userspace Programs (`system/userspace/`)

The following programs are compiled as x86-64 ELF64 PIE binaries, wrapped with a `UAOS` header, and placed in `C:`:

- `hello` — proof-of-concept write test.
- `pwd` — print current working directory.
- `file` — identify file types (ASCII, ELF64, Amiga Hunk, UAOS wrapper, data).
- `strings` — extract printable strings (`-n` for minimum length).
- `find` — recursive directory search with `-name` and `-type` filters.
- `Guide` — AmigaGuide viewer using the GUI syscall interface.
- `echo` — print text to the shell (`STRING/F,NOLINE/S`).
- `type` — print file contents (`FILE,HEX/S,NUMBER/S,TO/K`).
- `dir` — list a directory (`DIR,ALL/S,DATES/S,INTER/S,KEYS/S,OPT/K`).
- `list` — detailed directory listing (`ALL/DATES/INTER/KEYS/NOHEAD/LFORMAT`).
- `makedir` — create a directory.
- `delete` — delete a file or directory (`FILE/A,ALL/S,QUIET/S,FORCE/S`).
- `rename` — rename or move a file.
- `copy` — copy a file or directory tree (`FROM/A,TO/A,ALL/S,CLONE/S,DATES/S,COM/S,QUIET/S,BUFFER/K/N`).
- `protect` — set file protection bits (`+/-[hsparwed]`, `ALL`, `QUIET`).
- `attr` — show file attributes and protection bits.
- `grep` — search file contents (`PATTERN/A,FILE,CI/S`).
- `sort` — sort lines of a file (`FILE,COL/K/N,CASE/S,NUMERIC/S`).
- `join` — concatenate multiple files.
- `search` — search files for text (`PATTERN/A,FILE,ALL/S,FROM/K,FILEPAT/K,CI/S`).
- `filenote` — set or show a file's comment (`FILE/A,COMMENT/F`).
- `more` — paginate file output one screen at a time.
- `avail` — show available system memory (`BYTES/S`, `K/S`). Queries the kernel memory API via `SYSCALL_MEMINFO` and reports x86-64 heap total/used/free, M68k guest RAM slot usage, and scheduler task counts.

## GNU Core Utilities (`system/gnusrc/`)

The complete GNU coreutils set (86 utilities) is built as x86-64 ELF64 PIE binaries (same toolchain as the native `C:` commands) and staged into `SYS_ROOT/gnu/usr/bin/`. They use GNU-style flags via `uaos_getopt.h` and coexist with the AmigaDOS-style commands in `C:` (e.g., the Amiga `sort` remains in `C:` while the GNU `sort` lives in `gnu/usr/bin/`).

### Core Text Utilities
- `cat` — concatenate files (`-n`, `-b`, `-s`, `-A`, `-E`, `-T`, `-v`).
- `tac` — reverse line order (`-b`, `-s SEP`).
- `nl` — number lines (`-b`, `-v`, `-i`, `-w`, `-s`, `-n`).
- `wc` — count lines/words/bytes (`-c`, `-w`, `-l`, `-m`, `-L`).
- `head` — first lines (`-n N`, `-c N`, `-q`, `-v`, supports `+N`/`-N`).
- `tail` — last lines (`-n N`, `-c N`, `-q`, `-v`, supports `+N`/`-N`).
- `cut` — field/byte extraction (`-b`, `-c`, `-f`, `-d`, `-s`, `--complement`).
- `tr` — translate/delete characters (`-d`, `-s`, `-c`, ranges and escapes).
- `uniq` — adjacent duplicate removal (`-c`, `-d`, `-u`, `-i`, `-f`, `-s`, `-w`).
- `fold` — wrap long lines (`-b`, `-s`, `-w N`).
- `expand` — tabs to spaces (`-i`, `-t LIST`).
- `unexpand` — spaces to tabs (`-a`, `-f`, `-t LIST`).

### Advanced Text Utilities
- `paste` — merge file lines (`-d LIST`, `-s`).
- `comm` — compare sorted files (`-1`, `-2`, `-3`, `--output-delimiter`).
- `fmt` — simple paragraph formatter (`-w`, `-s`, `-c`, `-t`, `-u`).
- `sort` — sort lines (`-n`, `-r`, `-u`, `-f`, `-o`, `-t`, `-k`).
- `seq` — print number sequences (`-f`, `-s`, `-w`).
- `tsort` — topological sort from pair input.
- `shuf` — random permutations (`-i`, `-e`, `-n`, `-o`, `-r`, `-z`).
- `split` — split by lines/bytes (`-l`, `-b`, `-a`, `-d`, `--additional-suffix`).
- `csplit` — context-based split (`-f`, `-n`, `-k`, `-s`, `-z`, patterns).

### Encoding & Checksum Utilities
- `base32` — RFC 4648 base32 encode/decode (`-d`, `-w`, `-i`).
- `base64` — base64 encode/decode (`-d`, `-w`, `-i`).
- `basenc` — multi-encoding (`--base16`, `--base32`, `--base64`, `--base64url`, `-d`, `-w`).
- `od` — octal/decimal/hex/char dump (`-A`, `-t`, `-j`, `-N`, `-v`).
- `sum` — BSD/SysV checksum (`-r`, `-s`).
- `cksum` — POSIX CRC32 checksum and byte count (`--algorithm`).
- `md5sum` — MD5 digests (`-c`, `-z`).
- `sha1sum` — SHA-1 digests (`-c`, `-z`).
- `sha256sum` — SHA-256 digests (`-c`, `-z`).
- `sha512sum` — SHA-512 digests (`-c`, `-z`).
- `b2sum` — BLAKE2b digests (`-l N`, `-c`, `-z`).

### Other Text Utilities
- `pr` — paginate/columnate for printing (`-l`, `-w`, `-t`, `-N`, `-o`).
- `numfmt` — format numbers with SI/IEC suffixes (`--from`, `--to`, `--suffix`, `--padding`).
- `ptx` — permuted index / KWIC (`-G`, `-w`, `-A`, `-r`).

### File Listing & Information Utilities
- `ls` — list directory contents (`-l`, `-a`, `-A`, `-1`, `-r`, `-S`, `-t`, `-h`, `-R`, `-d`, `-F`, `-p`, `-n`).
- `dir` — like `ls -C -p` (column format with slash indicators).
- `vdir` — like `ls -l` (long format by default).
- `stat` — file or filesystem status (`-f`, `-c FORMAT`, `-t`).
- `df` — filesystem disk space usage (`-h`, `-k`, `-i`).
- `du` — estimate file space usage (`-h`, `-s`, `-a`, `-b`, `-k`).
- `basename` — strip directory and suffix (`-a`, `-s SUFFIX`, `-z`).
- `dirname` — strip last component (`-z`).
- `realpath` — resolved absolute path (`-e`, `-m`, `-q`, `-z`, `-s`).
- `pathchk` — check filename validity/portability (`-p`, `-P`).
- `mktemp` — create temporary file or directory (`-d`, `-q`, `-t`).

### File Manipulation Utilities
- `cp` — copy files and directories (`-r`, `-f`, `-i`, `-n`, `-v`, `-p`, `-u`).
- `mv` — move or rename files (`-f`, `-i`, `-n`, `-v`, `-u`).
- `rm` — remove files or directories (`-r`, `-f`, `-i`, `-v`, `-d`).
- `mkdir` — create directories (`-p`, `-m MODE`, `-v`).
- `rmdir` — remove empty directories (`-p`, `-v`, `--ignore-fail-on-non-empty`).
- `install` — copy files and set attributes (`-d`, `-m MODE`, `-v`, `-t DIR`, `-D`).
- `touch` — change file timestamps (`-a`, `-m`, `-c`, `-d STRING`, `-r FILE`).
- `truncate` — shrink/extend file size (`-s N`, `-c`, `-r FILE`).
- `shred` — securely overwrite files (`-n N`, `-u`, `-z`, `-v`, `-f`).
- `unlink` — remove a single file.
- `dd` — convert and copy a file (`if=`, `of=`, `bs=`, `count=`, `skip=`, `seek=`, `conv=`).

### Shell Basic Utilities
- `echo` — display a line of text (`-n`, `-e`, `-E`).
- `printf` — format and print data (`%s`, `%d`, `%x`, `%o`, `%c`, `%f`, `%b`).
- `yes` — output a string repeatedly.
- `true` — exit with status 0.
- `false` — exit with status 1.
- `test` / `[` — evaluate expression (`-e`, `-f`, `-d`, `-r`, `-w`, `-x`, `-s`, `-z`, `-n`, `=`, `!=`, `-eq`, `-ne`, `-lt`, `-le`, `-gt`, `-ge`, `!`, `-a`, `-o`).
- `expr` — evaluate expressions (arithmetic, string, comparison).
- `factor` — print prime factors (`--exponents`).
- `sleep` — delay for a duration (`s`, `m`, `h`, `d` suffixes).
- `tee` — read stdin, write to stdout and files (`-a`, `-i`, `-p`).
- `date` — print/set system date (`-u`, `-d STRING`, `+FORMAT`).
- `env` — run a command in a modified environment (`-i`, `-u NAME`, `-C DIR`).
- `printenv` — print environment variables (`-0`).

### System Information Utilities
- `uname` — system information (`-a`, `-s`, `-n`, `-r`, `-v`, `-m`, `-p`, `-i`, `-o`).
- `arch` — print machine hardware name.
- `nproc` — print number of processing units (`--all`, `--ignore=N`).
- `hostname` — show/set system host name (`-f`, `-s`, `-i`, `-d`).
- `hostid` — print numeric host identifier.
- `tty` — print terminal name (`-s`, `-q`).
- `whoami` — print effective user ID.
- `logname` — print login name.
- `id` — print user/group information (`-u`, `-g`, `-G`, `-n`, `-r`, `-z`).
- `groups` — print group memberships.
- `who` — show who is logged on (`-a`, `-b`, `-q`, `-H`).
- `users` — print logged-in user names.
- `pinky` — lightweight who utility (`-l`, `-f`, `-w`, `-i`, `-q`).

### User/Group Utilities
- `chmod` — change file permissions (`-R`, `-v`, `-c`, `-f`; octal and symbolic modes mapped to AmigaDOS protection bits).
- `chown` — change file owner (`-R`, `-v`, `-c`, `-f`; single-user OS — accepts args, no-op).
- `chgrp` — change file group (`-R`, `-v`, `-c`, `-f`; single-user OS — accepts args, no-op).

## M68k Demo Programs (`system/Demos/`)

Programs written in M68k assembly are assembled with `vasm` (Motorola syntax), linked into standard Amiga Hunk executables with `vlink`, and wrapped with the `UAOS` header by `gen_uaos_m68k`. They are staged into `SYS_ROOT/Demos/`:

- `CopperBars` — opens an Intuition window (with `WFLG_GIMMEZEROZERO`) and renders animated horizontal colour bars using `graphics.library` `RectFill` within the window's RastPort. Six bars in fixed Amiga palette colours bounce vertically inside the content area. It demonstrates `graphics.library` (`SetAPen`, `SetDrMd`, `RectFill`, `WaitTOF`) and `intuition.library` (`OpenWindow`, `CloseWindow`, `ModifyIDCMP`), and exits cleanly when the close gadget is clicked. The demo draws directly to the window RastPort instead of taking over the Copper, so it coexists with the desktop without starving the idle/WM task.

## Development Tools

### ACE Basic (`SYS:ACE/`)

ACE Basic 3.0.1 (GPL v2/v3) is a BASIC-to-M68k assembly compiler that ships as pre-built Amiga Hunk binaries. It is downloaded and staged at build time by `scripts/build_iso.sh` (Step 2i). The `bas` script in `C:` orchestrates compilation by invoking the ACE tools (`ace`, `yap`, `vasmm68k_mot`, `vlink`, `parseusing`) with the correct options and library paths.

- **`ACE:`** — points to `SYS:ACE` (the compiler installation root)
- **`ACElib:`** — points to `ACE:lib` (runtime library stubs)
- **`ACEbmaps:`** — points to `ACE:bmaps` (browser maps for intuition/graphics)
- **`ACEinclude:`** — points to `ACE:include` (header files)
- **`ACEsubmods:`** — points to `ACE:submods` (submodule stubs)

Usage: `bas hello.b` compiles `hello.b` to a native M68k executable. The compiled program runs directly under UAOS's 68020 emulation.

### Regina Rexx (`SYS:REXX/`)

Regina Rexx 0.08i (LGPL v2) is an ARexx-compatible interpreter that ships as a pure 68000 Amiga Hunk binary. It is downloaded and staged at build time by `scripts/build_iso.sh` (Step 2h). The native `C:rx` command wraps the interpreter, supporting both inline programs (`rx "say 'Hello'"`) and file-based programs (`rx myscript`).

- **`REXX:`** — points to `SYS:REXX` (interpreter binary and script directory)

The `bas` script uses `rx` for inline string manipulation (checking prefixes, translating characters, detecting option patterns).

## Startup Sequence

`system/Startup-Sequence` is an Amiga-style boot script that runs after kernel initialization. It sets up assigns (`ENV:`, `T:`, `Clips:`, `REXX:`, `LIBS:`), environment variables, network startup (`C:NetStart`), and optionally executes `S:User-Startup` before loading the Workbench with `C:LoadWB`.

Configuration files in `S:` include:

- `net.conf` — DHCP or static IPv4 configuration.
- `ntp.conf` — NTP server (default `pool.ntp.org`).
- `timezone.conf` — IANA timezone (default `Australia/Sydney`).
- `vim.conf` — Vim editor settings (`tabstop`, `number`, `hlsearch`, etc.).

## Filesystem Skeleton

The `system/` directory in the repository contains the template for the UAOS system root, which is packaged into the boot ISO by `scripts/build_iso.sh`.
