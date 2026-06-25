---
type: Kernel Library
title: bsdsocket.library
description: UAOS native implementation of the AmigaOS bsdsocket.library for emulated M68k tasks.
resource: /kernel/exec/bsdsocket_lib.c
tags: [bsdsocket, network, tcp, udp, m68k, thunking]
timestamp: 2026-06-24T17:00:00Z
---

# bsdsocket.library

`bsdsocket.library` provides a BSD-style socket API to emulated M68k tasks. The UAOS implementation is a native thunk layer that maps guest socket calls to the native TCP/IP stack in `kernel/net/`.

## Key Files

- `kernel/exec/bsdsocket_lib.c` — native implementations and function table.
- `kernel/exec/bsdsocket_lib.h` — guest socket structures and constants.
- `emulation/uaos_m68k_glue.c` — M68k LVO stub installation and dispatch.

## Implementation Status

### Socket Lifecycle

| Function | Status | Notes |
|---|---|---|
| `socket` | Implemented | Creates a native TCP or UDP socket and returns a guest socket descriptor. |
| `CloseSocket` | Implemented | Closes the native socket and frees the descriptor slot. |
| `bind` | Implemented | Binds a socket to a local address/port. |
| `connect` | Implemented | Initiates a TCP connection or sets default UDP peer. |
| `listen` | Implemented | Puts a TCP socket into listening state. |
| `accept` | Implemented | Accepts an incoming TCP connection. |

### Data Transfer

| Function | Status | Notes |
|---|---|---|
| `send` | Implemented | Sends data on a connected TCP socket. |
| `recv` | Implemented | Receives data from a TCP socket. |
| `sendto` | Implemented | Sends a UDP datagram to a specific address. |
| `recvfrom` | Implemented | Receives a UDP datagram and records the source address. |

### Name Resolution and Addressing

| Function | Status | Notes |
|---|---|---|
| `gethostbyname` | Implemented | Resolves a hostname to an IPv4 address using the native DNS resolver. |
| `inet_addr` | Implemented | Converts a dotted-decimal IPv4 string to network byte order. |
| `inet_ntoa` | Implemented | Converts an IPv4 address to a dotted-decimal string using a static buffer. |

### Socket Options and Control

| Function | Status | Notes |
|---|---|---|
| `setsockopt` | Implemented / Stub | Recognises common options; many are no-ops because the native stack does not require them. |
| `IoctlSocket` | Implemented / Stub | Supports common control requests; unrecognised requests return an error. |

## Descriptor Mapping

The guest socket descriptor table reserves 16 entries: descriptors `0`–`7` are TCP sockets and `8`–`15` are UDP sockets. When a guest `socket()` call arrives, the thunk layer creates a native TCP or UDP socket through `kernel/net/tcp.c` or `kernel/net/udp.c`, stores the native handle, and returns the guest descriptor.

Guest `sockaddr_in` structures are translated between the emulated M68k address space and native `struct sockaddr_in` layouts so that IP addresses and ports are passed correctly in both directions.

## Integration with the Native Stack

`bsdsocket.library` does not implement TCP or UDP itself; it delegates to:

- `tcp.c` for connection setup, state machine, send/receive, and teardown.
- `udp.c` for datagram send/receive and ephemeral ports.
- `dns.c` for hostname resolution.
- `net_device.c` and the underlying e1000/VirtIO-Net drivers for actual frame transmission.

This keeps the heavy networking code native while allowing legacy Amiga TCP/IP software to run unmodified.
