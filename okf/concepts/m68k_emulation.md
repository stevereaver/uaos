---
type: Concept
title: M68k Emulation in UAOS
description: How UAOS executes classic Motorola 68020 code on an x86_64 host.
tags: [m68k, emulation, musashi, thunking, 68020]
timestamp: 2026-06-24T17:00:00Z
---

# M68k Emulation

UAOS is designed to run emulated M68k code seamlessly alongside native x86_64 code. This is achieved through a combination of CPU emulation and high-level library thunking.

## CPU Emulator: Musashi

UAOS uses the **Musashi** M68k emulator core. It is integrated into the kernel and runs emulated tasks within their own context. The Musashi configuration (`emulation/uaos_m68kconf.h`) targets the **Motorola 68020** CPU (with `M68K_EMULATE_020` enabled) and ILLEGAL/TRAP callbacks enabled. The FPU and PMMU remain disabled. The CPU type is set to `M68K_CPU_TYPE_68020` in both `uaos_m68k_glue.c` (for the global emulator context) and `exec_task.c` (for each per-task M68k context).

The 68020 enables 32-bit multiply/divide instructions (`mulsl`, `mulul`, `divsl`, `divul`) used by the ACE Basic toolchain (`vasmm68k_mot`, `vlink`, `ace`). All 68000 instructions are a subset of 68020, so existing M68k demos continue to run without modification.

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

Accesses to the Amiga custom chip/CIA window at guest physical `0x00B00000–0x00DFFFFF` are not satisfied from guest RAM. The Musashi memory callbacks in `emulation/uaos_m68k_glue.c` detect this range and forward reads and writes to the chipset emulator (`kernel/chipset/chip_emu.c`), using the same `chip_emu_read`/`chip_emu_write` entry points as the native x86_64 page fault handler. This lets M68k code read registers such as `COP1LC` and write `DMACON`/`COLOR00` directly, which is required for copper-list based demos and games.

M68k tasks also receive a dedicated VBlank signal bit at creation (`UaosTask.m68k_vblank_sig`). `graphics.library/WaitTOF()` uses this signal to block the M68k task until the next VBlank instead of busy-waiting, preventing it from starving the lower-priority idle/WM task.

## Binary Loading

`uaos_m68k_glue.c` includes a minimal Amiga Hunk loader that supports `HUNK_CODE`, `HUNK_DATA`, `HUNK_BSS`, `HUNK_RELOC32`, `HUNK_RELOC16`, `HUNK_RELOC8`, `HUNK_SYMBOL`, `HUNK_DEBUG`, `HUNK_DREL32`, and `HUNK_RELOC32SHORT`. It loads each segment into the guest RAM program area and applies relocations so that raw Amiga binaries can be executed from the shell or from `dos.library/LoadSeg`.

The `HUNK_DREL32` and `HUNK_RELOC32SHORT` types use compact 16-bit relocation entries (count, target hunk, and offsets are all 16-bit). After processing these sections, the parser aligns to the next 4-byte boundary because subsequent hunk type words are always 32-bit.

## Per-Task Binary Loading (exec_task.c)

When the shell launches an M68k binary via `Task_CreateM68k`, the binary payload is copied into the tail of the task's guest RAM **before** the task starts running. This prevents the static `g_bin_payload` buffer from being overwritten by other tasks or by the guest RAM clear loop. The wrapper task (`m68k_wrapper_entry`) then clears the lower portion of guest RAM, installs library tables, and calls `hunk_load` with the saved copy.

Key startup conventions for per-task M68k execution:
- **A6** is pre-set to `EXEC_BASE` (0x300) — many programs (especially ACE-compiled binaries) expect SysBase in A6 without explicitly loading it from address 4.
- **A0** = command line pointer, **D0** = command line length (Amiga CLI convention).
- A **DOS_EXIT stub** return address is pushed onto the stack so that when the program does RTS at the end, it returns to the Exit handler and halts cleanly.
