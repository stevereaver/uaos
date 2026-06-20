---
type: Kernel Subsystem
title: Interrupts and Hardware Abstraction
description: Low-level interrupt handling, IDT management, and basic hardware drivers.
resource: /kernel/irq/
tags: [irq, idt, pic, drivers]
timestamp: 2026-06-18T10:00:00Z
---

# Interrupts and Hardware Abstraction

UAOS manages hardware interrupts and basic device drivers to provide a foundation for higher-level subsystems.

## Interrupt Descriptor Table (IDT)

The kernel sets up a 256-vector IDT in 64-bit mode.
- **Exceptions**: Vectors 0-31 handle CPU exceptions (e.g., Page Faults, GPF).
- **IRQs**: Hardware interrupts are remapped via the 8259A PIC to vectors 32-47.

## Core Hardware Drivers

- **PS/2 Keyboard (IRQ1)**: Handles scancode translation and provides a ring buffer for keystrokes.
- **PS/2 Mouse (IRQ12)**: Handles relative motion packets and updates the software cursor.
- **RTC (IRQ8)**: CMOS Real-Time Clock for system time and periodic interrupts (used by `timer.device`).
- **VirtIO Block**: Basic driver for VirtIO-compliant storage devices.

## Interrupt Handlers

Interrupt stubs are written in assembly (`idt_stubs.asm`) to save/restore registers and then call C handlers in `idt.c` or specific driver files.
