---
type: Kernel Subsystem
title: Exec Library
description: The core system library for task management, memory, and signals.
resource: /kernel/exec/
tags: [exec, tasks, memory, ipc]
timestamp: 2026-06-24T17:00:00Z
---

# Exec Library

The Exec library is the central "kernel" library in UAOS, following the design of Amiga's `exec.library`. It manages the most fundamental aspects of the system.

## Key Responsibilities

- **Task Management**: Creating, scheduling, and switching between tasks (both native x86_64 and emulated M68k).
- **Userspace Execution**: Running Ring-3 native x86-64 ELF64 tasks with memory protection (MMU sandboxing), parent-child hierarchy, and per-task current working directory.
- **System Calls**: Software interrupt `INT 0x80` register-based dispatcher facilitating kernel services for userspace programs.
- **Memory Allocation**: Managing the system memory map and providing allocation services.
- **IPC (Inter-Process Communication)**: Message ports and signals for communication between tasks (including `SIGF_CHILD` for parent notification).
- **Library Loading**: Dynamic loading of native and emulated libraries.
- **MMU Sandboxing**: 4-level paging and memory protection.

## Core Files

- `task.c`: Task creation (native, X64 user-space, and emulated M68k) and context switching logic.
- `exec_task.c`: AmigaOS-compatible `AddTask`/`FindTask`/`SetTaskPri` helpers for M68k tasks.
- `exec_signal.c`: Task lookup helpers for M68k guest process structures.
- `exec_ipc.c`: Message port and message passing implementation (`NewPort`, `PutMsg`, `GetMsg`, `WaitPort`, `ReplyMsg`).
- `syscall_dispatch.c`: Handling register-based system call routing and implementation for Ring-3 userspace programs.
- `elf64_loader.c`: ELF64 PIE/EXEC loader for native x86-64 userspace binaries.
- `loadable_lib.c`: Scans `Workbench:LIBS/` for loadable Amiga `.library` files and registers them with the emulation layer.
- `mmu_sandbox.c`: Paging and memory protection setup for the 4 GB Amiga address space.
- `page_fault_handler.c`: Handles page faults, including custom chip-window accesses from M68k code.
- `rom_modules.c`: Registers the built-in AmigaOS-compatible libraries at boot.
- `thunk_handler.c`: Native ABI thunk translator for `ILLEGAL` opcode breakout from M68k code.

## AmigaOS-Compatible Libraries and Devices

UAOS provides native thunk implementations of classic AmigaOS libraries and devices for emulated M68k tasks:

- [graphics.library](graphics_library.md) — drawing primitives, fonts, RastPorts, BitMaps, and View/ViewPort management.
- [intuition.library](intuition_library.md) — windows, screens, gadgets, IDCMP, menus, and BOOPSI dispatch.
- [gadtools.library](gadtools_library.md) — high-level gadget creation (buttons, checkboxes, sliders, string/integer gadgets, listviews, cycle gadgets) and layout helpers.
- [dos.library](/kernel/dos/index.md) — file I/O, directories, processes, and AmigaDOS packets.
- [workbench.library](workbench_library.md) — app icons, app windows, and Workbench integration.
- [bsdsocket.library](bsdsocket_library.md) — BSD socket API mapped to the native TCP/IP stack.
- [Other Libraries & Devices](other_libraries.md) — `utility.library`, `mathffp.library`, `locale.library`, `ixemul.library`, `console.device`, `keyboard.device`, `timer.device`.

## Task Stack Alignment

The x86-64 SysV ABI requires the stack pointer to be 16-byte aligned *before* a `CALL` instruction, which means a function is entered with `%rsp` 8-byte misaligned (the return address pushed by `CALL` makes it 16-byte aligned). To preserve this invariant across context switches, the kernel stacks are aligned to 8-byte boundaries (not 16-byte), and the synthetic interrupt frames built by `Task_CreateNative()` and `Task_CreateX64()` are sized so that the `iretq` epilogue leaves the new task with the ABI-required 8-byte misaligned `%rsp`.

Key details:
- Per-task kernel stacks are declared with `__attribute__((aligned(8)))`.
- Synthetic frames are 176 bytes for Ring-3 tasks (X64 ELF64) and 160 bytes for Ring-0 native tasks, matching the `iretq` pop count.
- `isr_common` and `uaos_syscall_isr` use the same frame layout for both the interrupted task and the task being switched to; no padding is inserted into the synthetic frame, keeping the layout identical to a CPU-generated interrupt frame.

## M68k Integration

Exec provides the bridge for emulated M68k tasks, including "LVO" (Library Vector Offset) stubs that allow M68k code to call native C functions.
