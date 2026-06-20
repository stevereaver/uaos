---
type: Concept
title: Thunking and LVOs
description: The mechanism for calling native x86_64 functions from emulated M68k code.
tags: [thunking, lvo, traps, interop]
timestamp: 2026-06-18T10:00:00Z
---

# Thunking and LVOs

**Thunking** is the process of translating calls between the emulated M68k environment and the native x86_64 kernel.

## Library Vector Offsets (LVOs)

In AmigaOS, libraries are accessed via a jump table at negative offsets from a library base pointer.
- `Open` is usually at `-30`.
- `Close` is usually at `-36`.

UAOS emulates this by placing special trap opcodes at these offsets in the emulated memory.

## The Thunking Process

1. **Call**: M68k code executes `jsr -30(a6)` where `a6` is the `dos.library` base.
2. **Trap**: The emulator reads the instruction at that address, which is a pre-placed `ILLEGAL` opcode.
3. **Intercept**: The kernel's trap handler is invoked.
4. **Parameter Marshalling**:
   - The handler reads M68k registers (e.g., `d1` for filename, `d2` for mode).
   - It translates emulated addresses to native x86_64 pointers.
5. **Execution**: The native C function `DOS_Open()` is called.
6. **Return**: The result (e.g., a file handle) is placed in the emulated `d0` register.
7. **Resume**: The emulator continues execution after the trap.

## Benefits

- **Performance**: Heavy lifting (like filesystem logic) is done natively at full x86_64 speed.
- **Compatibility**: Legacy M68k binaries can use modern kernel features seamlessly.
