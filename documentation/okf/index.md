---
type: OS Architecture
title: Ultimate Amiga OS (UAOS) Architecture
description: A comprehensive guide to the architecture of UAOS, a hobby operating system inspired by AmigaOS.
tags: [architecture, uaos, overview]
timestamp: 2026-06-18T10:00:00Z
---

# Ultimate Amiga OS (UAOS) Architecture

Welcome to the UAOS Architecture library. This documentation is organized using the Open Knowledge Format (OKF) to provide a structured, agent-friendly overview of the system.

## Core Subsystems

- [Kernel Architecture](/kernel/index.md) - The heart of UAOS.
- [M68k Emulation](/concepts/m68k_emulation.md) - How UAOS runs classic Amiga binaries.
- [Handler System](/kernel/dos/handler_system.md) - Packet-based I/O and filesystem architecture.
- [Window Manager](/kernel/display/index.md) - The graphical user interface and desktop.
- [Build System](/build_system.md) - How the OS is compiled and packaged.

## Directory Structure Overview

- `kernel/`: Core OS components (Exec, DOS, Display, etc.)
- `emulation/`: M68k CPU emulation and glue logic.
- `drivers/`: Hardware-specific drivers.
- `system/`: System layout (Amiga-style `S:`, `L:`, `LIBS:`, etc.)

## Key Principles

1. **Amiga Inspiration**: Aesthetics and architecture inspired by Workbench 3.x.
2. **Hybrid Execution**: Native x86_64 kernel running M68k code via emulation with "thunking" to native libraries.
3. **Packet-based I/O**: Asynchronous message passing for filesystem and device operations.
