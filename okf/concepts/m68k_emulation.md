---
type: Concept
title: M68k Emulation in UAOS
description: How UAOS executes classic Motorola 68000 code on an x86_64 host.
tags: [m68k, emulation, musashi, thunking]
timestamp: 2026-06-18T10:00:00Z
---

# M68k Emulation

UAOS is designed to run emulated M68k code seamlessly alongside native x86_64 code. This is achieved through a combination of CPU emulation and high-level library thunking.

## CPU Emulator: Musashi

UAOS uses the **Musashi** M68k emulator core. It is integrated into the kernel and runs emulated tasks within their own context.

## LVO Stubs and Thunking

To allow emulated M68k programs to call system functions (like `dos.library/Open`), UAOS uses **Library Vector Offset (LVO)** stubs.

1. **Trap**: The M68k code calls a negative offset from a library base.
2. **Intercept**: The emulator encounters a special "trap" instruction (e.g., `ILLEGAL` or a specific opcode).
3. **Dispatch**: The `uaos_m68k_glue.c` logic identifies the library and function being called.
4. **Native Execution**: The corresponding native C function in the kernel is executed.
5. **Return**: The result is placed in the emulated D0 register, and execution resumes in the M68k context.

## Memory Mapping

The M68k address space is mapped into the x86_64 address space, allowing the emulator to access both emulated memory and shared system structures.
