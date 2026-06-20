---
type: Kernel Subsystem
title: Exec Library
description: The core system library for task management, memory, and signals.
resource: /kernel/exec/
tags: [exec, tasks, memory, ipc]
timestamp: 2026-06-20T15:30:00Z
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
- `syscall_dispatch.c`: Handling register-based system call routing and implementation.
- `exec_ipc.c`: Message port and message passing implementation.
- `loadable_lib.c`: Infrastructure for loading and managing UAOS libraries.
- `mmu_sandbox.c`: Paging and memory protection setup.

## Task Stack Alignment

The x86-64 SysV ABI requires the stack pointer to be 16-byte aligned *before* a `CALL` instruction, which means a function is entered with `%rsp` 8-byte misaligned (the return address pushed by `CALL` makes it 16-byte aligned). To preserve this invariant across context switches, the kernel stacks are aligned to 8-byte boundaries (not 16-byte), and the synthetic interrupt frames built by `Task_CreateNative()` and `Task_CreateX64()` are sized so that the `iretq` epilogue leaves the new task with the ABI-required 8-byte misaligned `%rsp`.

Key details:
- Per-task kernel stacks are declared with `__attribute__((aligned(8)))`.
- Synthetic frames are 176 bytes for Ring-3 tasks (X64 ELF64) and 160 bytes for Ring-0 native tasks, matching the `iretq` pop count.
- `isr_common` and `uaos_syscall_isr` use the same frame layout for both the interrupted task and the task being switched to; no padding is inserted into the synthetic frame, keeping the layout identical to a CPU-generated interrupt frame.

## M68k Integration

Exec provides the bridge for emulated M68k tasks, including "LVO" (Library Vector Offset) stubs that allow M68k code to call native C functions.
