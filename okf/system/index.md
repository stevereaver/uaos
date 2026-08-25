---
type: System Layout
title: UAOS System Layout
description: Overview of the Amiga-style system directory structure in UAOS.
tags: [layout, filesystem, workbench]
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

## Userspace Library (`system/libuaos/`)

Native Ring-3 programs are built against a minimal freestanding library in `system/libuaos/`:

- **`uaos_start.c`**: Entry point (`_start`) that parses arguments and calls `main`.
- **`uaos_libc.h`**: Header-only static-inline implementations of `strlen`, `strcmp`, `strcpy`, `memcpy`, `memset`, `strlcat`, and character helpers.
- **`uaos_syscall.h`**: Inline wrappers for every `INT 0x80` syscall and the `uaos_gui_event`, `uaos_dirent`, and `uaos_stat` structures. Extended in Phase 7 with VFS mutation/metadata syscalls (0x20–0x2C): `mkdir`, `delete`, `rename`, `setprotection`, `getprotection`, `getcomment`, `setcomment`, `getvolumeinfo`, `readkey`, `getattrs`, `setattrs`, `getmountcount`, `getmountname`. Also defines AmigaDOS `FIBF_*` protection bit constants and `UAOS_ATTR_*` flags.
- **`uaos_template.h`**: Header-only AmigaDOS-style command template parser (ported from `kernel/shell/cmd_template.c`). Supports `/A`, `/K`, `/S`, `/N`, `/M`, `/F` qualifiers with `uaos_tmpl_parse()`, `uaos_tmpl_match()`, and query helpers (`uaos_tmpl_switch()`, `uaos_tmpl_string()`, `uaos_tmpl_int()`, `uaos_tmpl_count()`, `uaos_tmpl_multi()`).
- **`uaos_cmd.h`**: Shared helpers for userspace C: command binaries — output (`put_s`, `put_c`, `put_line`), argument reconstruction (`cmd_build_args`), path resolution (`cmd_make_abs`, `cmd_join_path`, `cmd_split_path_pat`), AmigaDOS pattern matching (`cmd_pattern_match` with `#?`, `?`, `*`, `%` wildcards), keyword helpers (`cmd_kw_find`, `cmd_kw_strip`), numeric formatting, and date formatting (`cmd_fmt_mtime`).

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

## M68k Demo Programs (`system/Demos/`)

Programs written in M68k assembly are assembled with `vasm` (Motorola syntax), linked into standard Amiga Hunk executables with `vlink`, and wrapped with the `UAOS` header by `gen_uaos_m68k`. They are staged into `SYS_ROOT/Demos/`:

- `CopperBars` — opens an Intuition window (with `WFLG_GIMMEZEROZERO`) and renders animated horizontal colour bars using `graphics.library` `RectFill` within the window's RastPort. Six bars in fixed Amiga palette colours bounce vertically inside the content area. It demonstrates `graphics.library` (`SetAPen`, `SetDrMd`, `RectFill`, `WaitTOF`) and `intuition.library` (`OpenWindow`, `CloseWindow`, `ModifyIDCMP`), and exits cleanly when the close gadget is clicked. The demo draws directly to the window RastPort instead of taking over the Copper, so it coexists with the desktop without starving the idle/WM task.

## Startup Sequence

`system/Startup-Sequence` is an Amiga-style boot script that runs after kernel initialization. It sets up assigns (`ENV:`, `T:`, `Clips:`, `REXX:`, `LIBS:`), environment variables, network startup (`C:NetStart`), and optionally executes `S:User-Startup` before loading the Workbench with `C:LoadWB`.

Configuration files in `S:` include:

- `net.conf` — DHCP or static IPv4 configuration.
- `ntp.conf` — NTP server (default `pool.ntp.org`).
- `timezone.conf` — IANA timezone (default `Australia/Sydney`).
- `vim.conf` — Vim editor settings (`tabstop`, `number`, `hlsearch`, etc.).

## Filesystem Skeleton

The `system/` directory in the repository contains the template for the UAOS system root, which is packaged into the boot ISO by `scripts/build_iso.sh`.
