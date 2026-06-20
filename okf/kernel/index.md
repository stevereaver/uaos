---
type: Kernel Subsystem
title: UAOS Kernel
description: Overview of the UAOS Kernel architecture, entry points, and initialization.
tags: [kernel, x86_64, boot]
timestamp: 2026-06-18T10:00:00Z
---

# UAOS Kernel

The UAOS kernel is a bare-metal x86_64 kernel that boots via GRUB2 Multiboot2. It initializes the system from 32-bit protected mode into 64-bit long mode.

## Initialization Flow

1. **`uaos_kernel_entry.asm`**: Sets up GDT, transitions to Long Mode, and calls `uaos_kernel_main`.
2. **`uaos_kernel_main.c`**:
   - `FB_Init()`: Initializes the linear framebuffer.
   - `IDT_Init()`: Sets up the Interrupt Descriptor Table and PIC.
   - `VFS_Init()`: Initializes the Virtual File System and RAM disk.
   - `UAOS_ROM_RegisterAll()`: Registers native implementations of Amiga libraries.
   - `Desktop_Draw()`: Renders the initial workbench.
   - **Event Loop**: Handles mouse and keyboard interrupts, routing them to the Window Manager.

## Subsystems

- [Exec Library](/kernel/exec/index.md): Task management, memory allocation, and IPC.
- [DOS Library](/kernel/dos/index.md): File system operations and handler management.
- [Display & WM](/kernel/display/index.md): Framebuffer management and windowing system.
- [IRQ & Drivers](/kernel/irq/index.md): Interrupt handling and hardware abstraction.
