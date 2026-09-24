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
- **VirtIO Block (`virtio_blk.c`)**: Driver for VirtIO-compliant storage devices (legacy PCI 1af4:1001). Keeps a single in-flight transaction — all block I/O is serialized by `cli`/`sti` in `BlockDev_Read`/`BlockDev_Write` — and issues an `mfence` before device notify so descriptor/avail-ring stores are globally visible to the device. On request timeout the free descriptor index (`g_virtq_free_idx`) is reset so the next I/O doesn't start from a stale index. I/O buffers must be DMA-accessible (4K-aligned statics, not stack).
- **VirtIO-SCSI (`virtio_scsi.c`)**: VirtIO-SCSI block device driver supporting both the modern virtio 1.0+ transport (PCI 1af4:1048, used by VirtualBox's virtio-scsi controller) and the legacy transport (PCI 1af4:1004, used by older QEMU). Walks PCI vendor capabilities for the modern MMIO transport (common/notify/ISR/device config) or uses the legacy BAR0 I/O interface. Scans SCSI targets 0-1 with TEST UNIT READY (with spin-up retries for CD-ROMs) and INQUIRY to detect device type. Hard disks (INQUIRY type 0) are registered as `virtio0` with 512-byte sectors; CD/DVD-ROMs (INQUIRY type 5) are registered as `vio_cd0` with 2048-byte sectors. The boot code mounts Workbench: from `vio_cd0` via ISO 9660 when no IDE ATAPI CD-ROM is found, enabling the S:/C:/L:/LIBS:/DEVS: assign flow on VirtualBox configurations where the DVD is attached to the virtio-scsi controller. Disables MSI/MSI-X to use legacy INTx. `vio_device_init` polls `device_status` until it reads 0 after RESET (was a fixed ~1000-iteration delay; VirtualBox VirtIO can take ~10s to reset, so the driver could proceed on a still-reset device). INQUIRY port probes retry up to 3x with a settle delay, and `vio_scsi_bdev_read` retries 3x on submit/timeout like the write path — a single timed-out request during probing or auto-mount no longer drops the disk for the whole boot. Command completion uses an independent last-consumed used-index shadow; sampling `used_idx` only after notify raced with VirtualBox completing small requests immediately, causing a full timeout and mis-associated retries for one-sector FAT writes.
- **Intel e1000 (`kernel/drivers/e1000.c`)**: Intel 82540EM Gigabit Ethernet driver.
- **VirtIO-Net (`kernel/drivers/virtio_net.c`)**: VirtIO legacy network device driver.
- **IDE (`kernel/drivers/ide.c`)**: ATA/ATAPI block device driver for CD-ROM and hard disks.

## Interrupt Handlers

Interrupt stubs are written in assembly (`idt_stubs.asm`) to save/restore registers and then call C handlers in `idt.c` or specific driver files.

All 256 stubs converge on `isr_common`, which pushes the general-purpose registers, dispatches to the C handler, and optionally performs a task switch by loading `native_rsp` from the target `UaosTask`. The `INT 0x80` syscall path uses a dedicated `uaos_syscall_isr` stub with the same frame layout so that scheduler-driven context switches can reuse the same `iretq` restore path. The synthetic interrupt frames built in `task.c` match this exact layout, ensuring that both the interrupted task and the newly-selected task can be resumed with a single `iretq` epilogue.
