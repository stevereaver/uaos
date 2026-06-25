---
type: OS Architecture
title: Ultimate Amiga OS (UAOS) Architecture
description: A comprehensive guide to the architecture of UAOS, a hobby operating system inspired by AmigaOS.
tags: [architecture, uaos, overview]
timestamp: 2026-06-24T17:00:00Z
---

# Ultimate Amiga OS (UAOS) Architecture

Welcome to the UAOS Architecture library. This documentation is organized using the Open Knowledge Format (OKF) to provide a structured, agent-friendly overview of the system.

## Core Subsystems

- [Kernel Architecture](/kernel/index.md) - The heart of UAOS.
- [Exec Library](/kernel/exec/index.md) - Task scheduling, memory, IPC, and AmigaOS-compatible libraries.
- [Userspace & Syscalls](/concepts/userspace_syscalls.md) - The Ring-3 task execution model and syscall interface.
- [M68k Emulation](/concepts/m68k_emulation.md) - How UAOS runs classic Amiga binaries.
- [Handler System](/kernel/dos/handler_system.md) - Packet-based I/O and filesystem architecture.
- [Window Manager](/kernel/display/index.md) - The graphical user interface and desktop.
- [TCP/IP Network Stack](/kernel/net/index.md) - IPv4 networking, DHCP, DNS, NTP, and socket API.
- [Build System](/build_system.md) - How the OS is compiled and packaged.

## Cross-Cutting Concepts

- [Icons](/concepts/icons.md) - Amiga `.info` icon loading, selected-state rendering, and desktop selection interaction.
- [Thunking and LVOs](/concepts/thunking.md) - Calling native libraries from emulated M68k code.
- [Concepts Index](/concepts/index.md) - All cross-cutting concepts.

## Directory Structure Overview

- `kernel/`: Core OS components (Exec, DOS, Display, IRQ, Net, Shell, Drivers).
- `emulation/`: M68k CPU emulation and glue logic.
- `drivers/`: Hardware-specific drivers (network, IDE, VirtIO).
- `system/`: System layout (Amiga-style `S:`, `L:`, `LIBS:`, userspace programs, and libuaos).

## Key Principles

1. **Amiga Inspiration**: Aesthetics and architecture inspired by Workbench 3.x.
2. **Hybrid Execution**: Native x86_64 kernel running M68k code via emulation with "thunking" to native libraries.
3. **Packet-based I/O**: Asynchronous message passing for filesystem and device operations.
