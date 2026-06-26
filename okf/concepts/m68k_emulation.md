---
type: Concept
title: M68k Emulation in UAOS
description: How UAOS executes classic Motorola 68000 code on an x86_64 host.
tags: [m68k, emulation, musashi, thunking]
timestamp: 2026-06-24T17:00:00Z
---

# M68k Emulation

UAOS is designed to run emulated M68k code seamlessly alongside native x86_64 code. This is achieved through a combination of CPU emulation and high-level library thunking.

## CPU Emulator: Musashi

UAOS uses the **Musashi** M68k emulator core. It is integrated into the kernel and runs emulated tasks within their own context. The Musashi configuration (`emulation/uaos_m68kconf.h`) targets plain M68000 with ILLEGAL and TRAP callbacks enabled and the FPU/PMMU disabled.

## LVO Stubs and Thunking

To allow emulated M68k programs to call system functions (like `dos.library/Open`), UAOS uses **Library Vector Offset (LVO)** stubs. At load time, small 4-byte `ILLEGAL` stubs (`0x4AFC` + 16-bit library/function ID) are written at the fixed jump-table addresses in guest RAM. When the M68k code calls a negative offset from a library base, the emulator hits the stub, the glue layer identifies the library and function, and the corresponding native C function is executed. The return value is placed in the emulated `D0` register before execution resumes.

1. **Trap**: The M68k code executes `jsr -xxx(a6)` where `a6` is a library base.
2. **Intercept**: The emulator encounters the `ILLEGAL` opcode at the LVO stub address.
3. **Dispatch**: `uaos_m68k_glue.c` identifies the library and function being called.
4. **Native Execution**: The native C function in the kernel runs (e.g., `DOS_Open()`).
5. **Return**: The result is placed in the emulated `D0` register and execution resumes.

For the low-level native ABI used by some ROM patches, see [Thunking](/concepts/thunking.md).

## Memory Mapping

The M68k address space is mapped into the x86_64 address space. The bare-metal build gives each M68k task a private 16 MB guest RAM pool (8 MB chip + 8 MB fast); the UAE bridge path allocates a 4 GB guest physical window and points the glue layer at offset 0. The first 16 MB of both layouts follow the same exception-vector / jump-table / stack / program layout so that Hunk binaries and library stubs can run in either environment.

## Binary Loading

`uaos_m68k_glue.c` includes a minimal Amiga Hunk loader that supports `HUNK_CODE`, `HUNK_DATA`, `HUNK_BSS`, and `HUNK_RELOC32`. It loads each segment into the guest RAM program area and applies relocations so that raw Amiga binaries can be executed from the shell or from `dos.library/LoadSeg`.
