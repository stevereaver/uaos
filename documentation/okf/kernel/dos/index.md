---
type: Kernel Subsystem
title: DOS Library & Handler System
description: The Amiga-inspired Disk Operating System and packet-based handler architecture.
resource: /kernel/dos/
tags: [dos, vfs, filesystem, handlers]
timestamp: 2026-06-18T10:00:00Z
---

# DOS Library & Handler System

The DOS library manages file system access and device I/O through a packet-based asynchronous architecture.

## Handler Architecture

In UAOS, filesystems and devices are managed by "Handlers". A handler is an object (often a separate task) that listens on a `MsgPort` for `DosPacket` messages.

### DosPacket Actions

Common actions include:
- `ACTION_FINDINPUT`: Open a file for reading.
- `ACTION_FINDOUTPUT`: Open a file for writing.
- `ACTION_READ`: Read data.
- `ACTION_WRITE`: Write data.

## Virtual File System (VFS)

The VFS layer provides a unified interface for multiple filesystems:
- **RAMFS**: An in-memory filesystem mounted at boot.
- **FAT32**: Support for physical disk partitions.
- **L: Directory**: Stores loadable handlers (e.g., `aux-handler`).

## Dynamic Handler Loading

Following the Amiga model, UAOS supports loading handlers from the `L:` directory. These handlers can be native or emulated M68k processes that handle specific device or filesystem logic.

For more details on the implementation plan, see [Handler System Plan](/HANDLER_SYSTEM_PLAN.md).
