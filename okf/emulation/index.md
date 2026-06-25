---
type: Emulation Layer
title: M68k Emulation Layer
description: Integration of the Musashi M68k emulator and the UAOS kernel.
resource: /emulation/
tags: [m68k, musashi, glue]
timestamp: 2026-06-24T17:00:00Z
---

# M68k Emulation Layer

The emulation layer provides the infrastructure to execute Motorola 68000 binary code on the x86_64 UAOS kernel.

## Core Components

- **`uaos_m68k_glue.c`**: The primary interface between the Musashi emulator and the kernel. It handles CPU initialization, memory access callbacks, opcode trapping, and the Amiga Hunk binary loader.
- **`uaos_emu_registry.c`**: Manages embedded Amiga binaries (e.g., `Lha`) and exposes `UAOS_Emu_RunByName()` for the shell.
- **`uaos_uae_bridge.c`**: UAE-compatible bridge that wires ILLEGAL callbacks and initializes the 4 GB guest physical RAM window used by some emulator builds.
- **`uaos_m68kconf.h`**: Musashi configuration tuned for M68000-only emulation with ILLEGAL/TRAP callbacks and no FPU/PMMU.
- **`rom_patches/`**: Contains assembly stubs (`rom_traps.s`) and the AROS kickstart configuration (`aros_kickstart.conf`).
- **`binaries/`**: Embedded Amiga binaries converted to C byte arrays (e.g., `Lha`).

## Execution Model

Emulated tasks are created as `UaosTask` objects with an M68k context. When the scheduler switches to an M68k task, it invokes the Musashi `m68k_execute()` function. The scheduler supports multiple M68k tasks, each with its own 2 MB guest RAM pool.

## Guest Memory Layout (Musashi Build)

Each M68k task receives a 2 MB guest RAM window mapped into the host address space:

| Region | Address Range | Purpose |
|---|---|---|
| Exception vectors | `0x000000`–`0x000100` | SSP at 0, PC at 4, plus initial stack pointer. |
| Library jump table | `0x000100`–`0x000200` | 4-byte `ILLEGAL` + `lib_id` stubs for LVO dispatch. |
| Stack | `0x000200`–`0x001000` | Initial stack grows down from `0x001000`. |
| Program segments | `0x001000`–`0x1FFFFF` | Loaded Amiga Hunk code/data/BSS segments. |

## Trap System

The emulation layer uses the `ILLEGAL` opcode to implement system calls (Traps). When the emulator encounters an `ILLEGAL` instruction, the glue logic checks the address to determine which LVO is being called. `TRAP #1` is used for simple DOS-style I/O (Write/Output, etc.).

## Embedded Binary Registry

`emulation/binaries/` holds Amiga binaries that are converted to C byte arrays by `scripts/embed_binary.sh`. The registry in `uaos_emu_registry.c` maps names (e.g., `Lha`) to the embedded data so the shell can run them with `C:run <name>`.

For details on how M68k code calls native functions, see [Thunking](/concepts/thunking.md).
