---
type: Kernel Subsystem
title: UAOS Kernel
description: Overview of the UAOS Kernel architecture, entry points, initialization, and subsystems including networking.
tags: [kernel, x86_64, boot]
timestamp: 2026-06-24T17:00:00Z
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
   - `Desktop_Draw()`: Renders the initial workbench backdrop and menu bar.
   - `S:Startup-Sequence` / `C:LoadWB`: Loads the full Workbench desktop.
   - **Event Loop**: Handles mouse and keyboard interrupts, routing them to the Window Manager and desktop.

## Subsystems

- [Exec Library](/kernel/exec/index.md): Task management, memory allocation, IPC, and AmigaOS-compatible library thunks.
- [DOS Library](/kernel/dos/index.md): File system operations and handler management.
- [Display & WM](/kernel/display/index.md): Framebuffer management and windowing system.
- [Chipset Emulator](/kernel/chipset/index.md): AGA/ECS custom chip register emulation and color palette.
- [IRQ & Drivers](/kernel/irq/index.md): Interrupt handling and hardware abstraction.
- [TCP/IP Network Stack](/kernel/net/index.md): IPv4 networking, DHCP, DNS, NTP, and socket API.
