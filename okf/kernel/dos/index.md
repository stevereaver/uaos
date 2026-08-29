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
- **CrossDOS (FAT12/16)**: Read-only support for PC-format floppy disks and small partitions (`kernel/dos/crossdos_handler.c`). Probes boot sector to distinguish FAT12 from FAT16, reads root directory and file cluster chains. Mounted via `crossdos` shell command.
- **PFS3**: Professional File System 3 support (`kernel/dos/pfs3.c`).
- **EXT4**: Read-only EXT4 support (`kernel/dos/ext4.c`).
- **ISO9660**: CD-ROM read support (`kernel/dos/iso9660.c`).

## Block Devices and Partitioning

- **Block device layer (`blockdev.c`)**: Unified interface for storage devices.
- **Partition table (`partition.c`)**: MBR parsing and partition registration.
- **IDE driver (`kernel/drivers/ide.c`)**: ATA/ATAPI PIO access for hard disks and CD-ROMs.
- **VirtIO Block (`virtio_blk.c`)**: VirtIO-compliant block device driver.

## Dynamic Handler Loading

Following the Amiga model, UAOS supports loading handlers from the `L:` directory. Built-in handlers include `ram_handler.c`, `fat_handler.c`, `device_handler.c`, `aux_handler.c`, `port_handler.c`, `print_handler.c`, and `crossdos_handler.c`. Loadable handlers can be native or emulated M68k processes that handle specific device or filesystem logic.

### Print Handler (`print_handler.c`)

The PRT: print handler extends the generic `DeviceHandler` with LPT1 hardware output. When data is written to PRT:, bytes are strobed out to the host parallel port at I/O address 0x378. If no LPT1 is detected (e.g., in QEMU), data is buffered in the device ring buffer. The handler is registered as `print-handler` in the handler loader and can be accessed via the `print` shell command.

### CrossDOS Handler (`crossdos_handler.c`)

The CrossDOS handler provides read-only access to PC-format (MS-DOS FAT12/FAT16) floppy disks and small partitions. It implements:
- Boot sector probing and FAT type detection (FAT12 < 4085 clusters, FAT16 < 65525 clusters)
- FAT12/FAT16 cluster chain following with proper entry decoding (12-bit packed / 16-bit entries)
- Root directory scanning with 8.3 filename matching
- File read via cluster chain traversal
- AmigaDOS packet interface (ACTION_FINDINPUT, ACTION_READ, ACTION_END, ACTION_DISK_INFO)

Mounted via the `crossdos` shell command, which probes a block device and registers the volume in the DosList.

For more details on the packet handler design, see [Handler System](handler_system.md).

## Icon Engine (`icon_loader.c`)

The icon engine handles Amiga `.info` file reading, writing, and default icon generation.

### Icon Reader (`Icon_Load`)
Parses classic planar `.info` files from VFS into `ParsedIcon` structures. Converts interleaved bitplane data to ARGB for the linear framebuffer. Supports normal and selected image states, tool types, and default tool strings.

### Icon Writer (`Icon_Save`)
Serializes a `ParsedIcon` back to the Amiga `.info` binary format. Converts ARGB pixels back to planar bitplanes using the 4-color Workbench palette (transparent/white/black/grey). Writes the full DiskObject header, Gadget, Image structs, planar data, default tool string, and tool type BPTR array.

### Snapshot Write-back (`Icon_SavePosition`)
Updates `do_CurrentX`/`do_CurrentY` in an existing `.info` file without rewriting the entire file. If the `.info` doesn't exist, creates a minimal one with just the position. Used by the Workbench Snapshot menu action.

### Tool Type API
- `Icon_ToolTypeGet(icon, key)` — finds a tool type by key prefix (e.g. `"STARTPRI"` matches `"STARTPRI=5"`)
- `Icon_ToolTypeSet(icon, key, value)` — sets or replaces a tool type entry
- `Icon_ToolTypeDelete(icon, key)` — removes a tool type entry

### Default Icons / Pseudo-Icons (`Icon_MakeDefault`)
Generates procedural 4-color (depth=2) 32x32 pixel icons for each Workbench type:
- `WB_DISK` — floppy disk shape with label area
- `WB_DRAWER` — folder with tab
- `WB_TOOL` — page with folded corner and gear
- `WB_PROJECT` — page with text lines
- `WB_GARBAGE` — trashcan with lid
- `WB_DEVICE` — device box with LED
- `WB_KICK` — chip with pins

Selected state is auto-generated by inverting white↔black. Used for files without a `.info` file (pseudo-icons).
