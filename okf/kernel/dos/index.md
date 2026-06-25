---
type: Kernel Subsystem
title: DOS Library & Handler System
description: The Amiga-inspired Disk Operating System and packet-based handler architecture.
resource: /kernel/dos/
tags: [dos, vfs, filesystem, handlers]
timestamp: 2026-06-24T17:00:00Z
---

# DOS Library & Handler System

The DOS library manages file system access and device I/O through a packet-based asynchronous architecture. The AmigaOS-compatible `dos.library` implementation lives in `kernel/exec/dos_lib.c`; the lower-level VFS, handlers, and filesystem drivers live in `kernel/dos/`.

## Handler Architecture

In UAOS, filesystems and devices are managed by "Handlers". A handler is an object that listens on a `MsgPort` for `DosPacket` messages.

### DosPacket Actions

Common actions include:
- `ACTION_FINDINPUT`: Open a file for reading.
- `ACTION_FINDOUTPUT`: Open a file for writing.
- `ACTION_READ`: Read data.
- `ACTION_WRITE`: Write data.
- `ACTION_EXAMINE`: Read a directory entry.
- `ACTION_EXAMINE_NEXT`: Read the next directory entry.
- `ACTION_FINDUPDATE`: Open an existing file for read/write.
- `ACTION_DELETE`: Delete a file or directory.
- `ACTION_CREATE_DIR`: Create a directory.

## Virtual File System (VFS)

The VFS layer (`kernel/dos/vfs.c`) provides a unified interface for multiple filesystems and the AmigaDOS assign system. Supported filesystems:

- **RAMFS**: An in-memory filesystem mounted at boot with `ENV:`, `T:`, `Clips:`, and `REXX:`.
- **FAT32**: Read/write support for physical disk partitions (`kernel/dos/fat32.c`, `fat_handler.c`).
- **PFS3**: Professional File System 3 support (`kernel/dos/pfs3.c`).
- **EXT4**: Read-only EXT4 support (`kernel/dos/ext4.c`).
- **ISO9660**: CD-ROM read support (`kernel/dos/iso9660.c`).

## Block Devices and Partitioning

- **Block device layer (`blockdev.c`)**: Unified interface for storage devices.
- **Partition table (`partition.c`)**: MBR parsing and partition registration.
- **IDE driver (`kernel/drivers/ide.c`)**: ATA/ATAPI PIO access for hard disks and CD-ROMs.
- **VirtIO Block (`virtio_blk.c`)**: VirtIO-compliant block device driver.

## Dynamic Handler Loading

Following the Amiga model, UAOS supports loading handlers from the `L:` directory. Built-in handlers include `ram_handler.c`, `fat_handler.c`, `device_handler.c`, `aux_handler.c`, and `port_handler.c`. Loadable handlers can be native or emulated M68k processes that handle specific device or filesystem logic.

For more details on the packet handler design, see [Handler System](handler_system.md).
