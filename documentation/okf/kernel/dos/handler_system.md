---
type: Concept
title: Handler System
description: Detailed view of the packet-based handler system in UAOS.
tags: [handlers, dospackets, msgports]
timestamp: 2026-06-18T10:00:00Z
---

# Handler System

The UAOS Handler System is a powerful abstraction for I/O, modeled after AmigaOS. It allows the kernel to treat filesystems, devices, and even network sockets as message-processing entities.

## Architecture

A **Handler** is an entity that processes `DosPacket` structures sent to its `MsgPort`.

```c
typedef struct Handler {
    MsgPort         port;       // Message port to receive packets
    const char     *name;       // Device name (e.g., "RAM", "AUX")
    void           *private;    // Internal state
    void (*ProcessPacket)(struct Handler *h, DosPacket *pkt);
} Handler;
```

## Packet Flow

1. **Client Request**: A task (e.g., the Shell) calls a `dos.library` function like `Open("RAM:test.txt")`.
2. **Library Dispatch**: `dos.library` looks up the handler for the "RAM:" device.
3. **Message Passing**: A `DosPacket` of type `ACTION_FINDINPUT` is constructed and sent to the handler's port.
4. **Handler Processing**: The handler (e.g., `ram_handler.c`) receives the packet, performs the I/O, and sets the results in the packet.
5. **Reply**: The packet is returned to the client's reply port.

## Filesystem vs. Device Handlers

- **Filesystem Handlers**: Manage directories and files (e.g., `RAMFS`, `FAT32`).
- **Device Handlers**: Manage character or block streams (e.g., `AUX:`, `CONSOLE:`).

## Future: Dynamic Handlers

The system is being expanded to support loading handlers from `L:` as separate M68k or native processes. See [Handler System Plan](/HANDLER_SYSTEM_PLAN.md) for the roadmap.
