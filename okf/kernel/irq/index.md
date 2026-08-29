---
type: Kernel Subsystem
title: Interrupts and Hardware Abstraction
description: Low-level interrupt handling, IDT management, and basic hardware drivers.
resource: /kernel/irq/
tags: [irq, idt, pic, drivers]
timestamp: 2026-06-24T17:00:00Z
---

# Interrupts and Hardware Abstraction

UAOS manages hardware interrupts and basic device drivers to provide a foundation for higher-level subsystems.

## Interrupt Descriptor Table (IDT)

The kernel sets up a 256-vector IDT in 64-bit mode.
- **Exceptions**: Vectors 0-31 handle CPU exceptions (e.g., Page Faults, GPF).
- **IRQs**: Hardware interrupts are remapped via the 8259A PIC to vectors 32-47.

## Core Hardware Drivers

- **PS/2 Keyboard (`ps2kbd.c`, IRQ1)**: Handles scancode set 1 translation and provides a ring buffer for keystrokes, including extended scancodes and modifier keys. Supports Amiga key mapping: Left Super/Windows → LAmiga, Right Super/Windows → RAmiga. RAmiga+letter pushes `0x80|UPPER` for menu shortcuts. LAmiga+V/B/M/N pushes special codes (`AMIGA_LV`/`AMIGA_LB`/`AMIGA_LM`/`AMIGA_LN`) for requester Verify/Cancel and screen cycling. The idle loop in `task.c` dispatches these: LAmiga+M/N calls `UAOS_Intuition_CycleScreen()`, RAmiga+letter calls `Intuition_InvokeCommandKey()`, and LAmiga+V/B is consumed (future: routed to active requester).
- **PS/2 Mouse (`ps2mouse.c`, IRQ12)**: Handles relative motion packets and updates the software cursor.
- **VMware Mouse (`vmmouse.c`)**: Optional absolute mouse driver using the VMware backdoor port (`0x5658`), enabled when running under QEMU/VMware.
- **RTC (`rtc.c`, IRQ8)**: CMOS Real-Time Clock for system time and periodic interrupts (used by `timer.device`).
- **VirtIO Block (`virtio_blk.c`)**: Basic driver for VirtIO-compliant storage devices.
- **Intel e1000 (`kernel/drivers/e1000.c`)**: Intel 82540EM Gigabit Ethernet driver.
- **VirtIO-Net (`kernel/drivers/virtio_net.c`)**: VirtIO legacy network device driver.
- **IDE (`kernel/drivers/ide.c`)**: ATA/ATAPI block device driver for CD-ROM and hard disks.

## Interrupt Handlers

Interrupt stubs are written in assembly (`idt_stubs.asm`) to save/restore registers and then call C handlers in `idt.c` or specific driver files.

All 256 stubs converge on `isr_common`, which pushes the general-purpose registers, dispatches to the C handler, and optionally performs a task switch by loading `native_rsp` from the target `UaosTask`. The `INT 0x80` syscall path uses a dedicated `uaos_syscall_isr` stub with the same frame layout so that scheduler-driven context switches can reuse the same `iretq` restore path. The synthetic interrupt frames built in `task.c` match this exact layout, ensuring that both the interrupted task and the newly-selected task can be resumed with a single `iretq` epilogue.
