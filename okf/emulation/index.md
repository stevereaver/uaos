---
type: Emulation Layer
title: M68k Emulation Layer
description: Integration of the Musashi M68k emulator and the UAOS kernel.
resource: /emulation/
tags: [m68k, musashi, glue]
timestamp: 2026-06-18T10:00:00Z
---

# M68k Emulation Layer

The emulation layer provides the infrastructure to execute Motorola 68000 binary code on the x86_64 UAOS kernel.

## Core Components

- **`uaos_m68k_glue.c`**: The primary interface between the Musashi emulator and the kernel. It handles CPU initialization, memory access callbacks, and opcode trapping.
- **`uaos_emu_registry.c`**: Manages the mapping of emulated resources and state.
- **`rom_patches/`**: Contains assembly stubs and configuration for the emulated environment (e.g., Kickstart-like vectors).

## Execution Model

Emulated tasks are created as `UaosTask` objects with an M68k context. When the scheduler switches to an M68k task, it invokes the Musashi `m68k_execute()` function.

## Trap System

The emulation layer uses the `ILLEGAL` opcode to implement system calls (Traps). When the emulator encounters an `ILLEGAL` instruction, the glue logic checks the address to determine which LVO is being called.

For details on how M68k code calls native functions, see [Thunking](/concepts/thunking.md).
