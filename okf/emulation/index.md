---
type: Emulation Layer
title: M68k Emulation Layer
description: Integration of the Musashi M68k emulator and the UAOS kernel.
resource: /emulation/
tags: [m68k, musashi, glue]
timestamp: 2026-08-30T15:00:00Z
---

# M68k Emulation Layer

The emulation layer provides the infrastructure to execute Motorola 68000 binary code on the x86_64 UAOS kernel.

## Core Components

- **`uaos_m68k_glue.c`**: The primary interface between the Musashi emulator and the kernel. It handles CPU initialization, memory access callbacks, opcode trapping, and the Amiga Hunk binary loader.
- **`uaos_emu_registry.c`**: Manages embedded Amiga binaries (e.g., `Lha`) and exposes `UAOS_Emu_RunByName()` for the shell.
- **`uaos_uae_bridge.c`**: UAE-compatible bridge that wires ILLEGAL callbacks and initializes the 4 GB guest physical RAM window used by some emulator builds.
- **`uaos_m68kconf.h`**: Musashi configuration tuned for M68020 emulation (the 68020 core is a superset of the 68000, so 68000 code runs unmodified) with ILLEGAL/TRAP callbacks and no FPU/PMMU. The CPU type is selected at runtime in `uaos_m68k_glue.c` via `m68k_set_cpu_type(M68K_CPU_TYPE_68020)`.
- **`rom_patches/`**: Contains assembly stubs (`rom_traps.s`) and the kickstart configuration (`kickstart.conf`).
- **`binaries/`**: Embedded Amiga binaries converted to C byte arrays (e.g., `Lha`).

## Execution Model

Emulated tasks are created as `UaosTask` objects with an M68k context. When the scheduler switches to an M68k task, it invokes the Musashi `m68k_execute()` function. The scheduler supports multiple M68k tasks, each with its own 16 MB guest RAM pool (8 MB chip + 8 MB fast).

## Guest Memory Layout (Musashi Build)

Each M68k task receives a 16 MB guest RAM window mapped into the host address space. The first 8 MB are chip RAM and the second 8 MB are fast RAM:

| Region | Address Range | Purpose |
|---|---|---|
| Exception vectors | `0x000000`–`0x000100` | SSP at 0, PC at 4, plus initial stack pointer. |
| Library jump table | `0x000100`–`0x000200` | 4-byte `ILLEGAL` + `lib_id` stubs for LVO dispatch. |
| Stack | `0x000200`–`0x001000` | Initial stack grows down from `0x001000`. |
| Program segments | `0x001000`–`0xFFFFFF` | Loaded Amiga Hunk code/data/BSS segments. |

Note: the upper 16 MB address range also contains the Amiga custom chip/CIA window at `0xB00000`–`0xDFFFFF`.  Accesses to this range are not satisfied from the guest RAM array; instead, the Musashi memory callbacks in `uaos_m68k_glue.c` route them to the chipset emulator (`chip_emu_read`/`chip_emu_write`), using the same entry points as the native x86_64 page fault handler.  This allows M68k code to read and write Amiga hardware registers directly.

M68k tasks are given a private VBlank signal bit (`UaosTask.m68k_vblank_sig`) so that `graphics.library/WaitTOF()` can block on `Wait()` instead of busy-waiting.  The VBlank path in `timer_ProcessTicks()` signals the waiting task, keeping the idle/WM task responsive while M68k animations run at ~50 Hz.

## Trap System

The emulation layer uses the `ILLEGAL` opcode to implement system calls (Traps). When the emulator encounters an `ILLEGAL` instruction, the glue logic checks the address to determine which LVO is being called. `TRAP #1` is used for simple DOS-style I/O (Write/Output, etc.).

## Embedded Binary Registry

`emulation/binaries/` holds Amiga binaries that are converted to C byte arrays by `scripts/embed_binary.sh`. The registry in `uaos_emu_registry.c` maps names (e.g., `Lha`) to the embedded data so the shell can run them with `C:run <name>`.

For details on how M68k code calls native functions, see [Thunking](/concepts/thunking.md).
