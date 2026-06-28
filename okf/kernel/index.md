---
type: Kernel Subsystem
title: UAOS Kernel
description: Overview of the UAOS Kernel architecture, entry points, initialization, and subsystems including networking.
tags: [kernel, x86_64, boot]
timestamp: 2026-06-28T17:47:00Z
---

# UAOS Kernel

The UAOS kernel is a bare-metal x86_64 kernel that boots via GRUB2 Multiboot2. It initializes the system from 32-bit protected mode into 64-bit long mode.

## Initialization Flow

1. **`uaos_kernel_entry.asm`**: Sets up GDT, transitions to Long Mode with a temporary 1 GB identity-mapped page table, and calls `uaos_kernel_main`.
2. **`uaos_kernel_main.c`**:
   - `FB_Init()`: Parses the Multiboot2 linear framebuffer tag (physical address, dimensions, pitch, bpp). The framebuffer address is used as-is and therefore relies on an identity mapping.
   - `chip_emu_reset()`: Resets the AGA/ECS chipset emulator state.
   - `UAOS_MMU_Init()`: Installs the 4 GB identity-mapped MMU sandbox. This must happen **before** any code writes to the linear framebuffer, because bootloaders may place the framebuffer above 1 GB (e.g. VirtualBox exposes the VGA LFB at `0x80000000`), while the bootstrap page tables only cover the first 1 GB. The chip/CIA register window at `0x00B00000-0x00DFFFFF` is intentionally left non-present and is handled by the page fault handler.
   - `audio_init()`: Initialises the host audio subsystem (AC97 / PC speaker fallback).
   - Chipset self-tests (`chip_emu_dma_test`, `chip_emu_line_test`, `chip_emu_fill_test`, `chip_emu_raster_test`, `chip_emu_sprite_test`). These render into the framebuffer and therefore require the MMU sandbox to be active.
   - `IDT_Init()` / `GDT_InitTSS()` / `PIC_Init()`: Set up interrupts, the TSS, and the 8259A PIC. After this, `uaos_page_fault_isr` is installed at IDT vector 14 to decode and forward Amiga chip-window accesses to the emulator.
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
- [Audio Subsystem](/kernel/audio/index.md): Paula mixer, ring buffer, AC97, and PC speaker fallback.
- [IRQ & Drivers](/kernel/irq/index.md): Interrupt handling and hardware abstraction.
- [TCP/IP Network Stack](/kernel/net/index.md): IPv4 networking, DHCP, DNS, NTP, and socket API.
