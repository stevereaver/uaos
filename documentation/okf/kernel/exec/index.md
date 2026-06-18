---
type: Kernel Subsystem
title: Exec Library
description: The core system library for task management, memory, and signals.
resource: /kernel/exec/
tags: [exec, tasks, memory, ipc]
timestamp: 2026-06-18T10:00:00Z
---

# Exec Library

The Exec library is the central "kernel" library in UAOS, following the design of Amiga's `exec.library`. It manages the most fundamental aspects of the system.

## Key Responsibilities

- **Task Management**: Creating, scheduling, and switching between tasks (both native x86_64 and emulated M68k).
- **Memory Allocation**: Managing the system memory map and providing allocation services.
- **IPC (Inter-Process Communication)**: Message ports and signals for communication between tasks.
- **Library Loading**: Dynamic loading of native and emulated libraries.
- **MMU Sandboxing**: 4-level paging and memory protection.

## Core Files

- `task.c`: Task creation and context switching logic.
- `exec_ipc.c`: Message port and message passing implementation.
- `loadable_lib.c`: Infrastructure for loading and managing UAOS libraries.
- `mmu_sandbox.c`: Paging and memory protection setup.

## M68k Integration

Exec provides the bridge for emulated M68k tasks, including "LVO" (Library Vector Offset) stubs that allow M68k code to call native C functions.
