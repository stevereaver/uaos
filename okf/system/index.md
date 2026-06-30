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
- **`uaos_syscall.h`**: Inline wrappers for every `INT 0x80` syscall and the `uaos_gui_event`, `uaos_dirent`, and `uaos_stat` structures.

## Native Userspace Programs (`system/userspace/`)

The following programs are compiled as x86-64 ELF64 PIE binaries, wrapped with a `UAOS` header, and placed in `C:`:

- `hello` — proof-of-concept write test.
- `pwd` — print current working directory.
- `file` — identify file types (ASCII, ELF64, Amiga Hunk, UAOS wrapper, data).
- `strings` — extract printable strings (`-n` for minimum length).
- `find` — recursive directory search with `-name` and `-type` filters.
- `Guide` — AmigaGuide viewer using the GUI syscall interface.

## M68k Demo Programs (`system/Demos/`)

Programs written in M68k assembly are assembled with `vasm` (Motorola syntax), linked into standard Amiga Hunk executables with `vlink`, and wrapped with the `UAOS` header by `gen_uaos_m68k`. They are staged into `SYS_ROOT/Demos/`:

- `CopperBars` — opens an Intuition window and renders animated horizontal raster bars by writing a custom Copper list to the chipset emulator. It demonstrates the use of `graphics.library` (`MakeVPort`, `MrgCop`, `LoadView`, `WaitTOF`) and `intuition.library` (`OpenWindow`, `CloseWindow`, `ModifyIDCMP`), and exits cleanly when the window is closed.

## Startup Sequence

`system/Startup-Sequence` is an Amiga-style boot script that runs after kernel initialization. It sets up assigns (`ENV:`, `T:`, `Clips:`, `REXX:`, `LIBS:`), environment variables, network startup (`C:NetStart`), and optionally executes `S:User-Startup` before loading the Workbench with `C:LoadWB`.

Configuration files in `S:` include:

- `net.conf` — DHCP or static IPv4 configuration.
- `ntp.conf` — NTP server (default `pool.ntp.org`).
- `timezone.conf` — IANA timezone (default `Australia/Sydney`).
- `vim.conf` — Vim editor settings (`tabstop`, `number`, `hlsearch`, etc.).

## Filesystem Skeleton

The `system/` directory in the repository contains the template for the UAOS system root, which is packaged into the boot ISO by `scripts/build_iso.sh`.
