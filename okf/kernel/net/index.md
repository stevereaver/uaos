---
type: Kernel Subsystem
title: TCP/IP Network Stack
description: The native IPv4 networking stack, device drivers, and higher-level protocols in UAOS.
resource: /kernel/net/
tags: [network, tcp, udp, ip, dhcp, dns, ntp]
timestamp: 2026-06-24T17:00:00Z
---

# TCP/IP Network Stack

UAOS includes a native IPv4 networking stack implemented in `kernel/net/`. It supports ARP, ICMP, UDP, TCP, DHCP, DNS, and NTP, and is exposed to emulated M68k programs through `bsdsocket.library`.

## Initialization

`net/stack.c` provides the top-level API. At boot (or on `C:NetStart`), the stack auto-probes for a network device:

1. Try Intel e1000 (`kernel/drivers/e1000.c`).
2. If no e1000 is found, try VirtIO-Net (`kernel/drivers/virtio_net.c`).

Once a device is registered through `netdev_register()`, the stack can send and receive Ethernet frames.

## Protocol Layers

### Ethernet (`eth.c`)

Simple RX dispatch: ARP frames (`0x0806`) go to the ARP handler; IPv4 frames (`0x0800`) go to the IP layer.

### ARP (`arp.c`)

Maintains a small ARP cache with simple LRU eviction. Sends ARP requests and replies, and updates the cache from incoming traffic. Used by the IP layer before sending to a non-local address.

### IPv4 (`ip.c`)

- Verifies header checksums.
- Drops fragmented packets (no reassembly).
- Handles local delivery, broadcast, and gateway routing.
- Dispatches to ICMP, UDP, or TCP based on the protocol field.
- Serial debug output is limited to errors only (bad length, bad checksum, fragments). Per-packet "rx proto" and "dispatching" logging was removed because it produced ~180k lines of blocking serial output at 115200 baud (~20 min of CPU time), which starved the PS/2 mouse IRQ (IRQ 12, lower priority than E1000's IRQ 11 on the slave 8259A PIC) and froze the UI.

### ICMP (`icmp.c`)

Implements echo request/reply (ping). The `ping` shell command uses this layer and waits for `icmp_got_reply()`.

### UDP (`udp.c`)

- Socket table with ephemeral port allocation (`49152`–`65535`).
- Ring-buffer RX path.
- Used by DHCP, DNS, and NTP.

### TCP (`tcp.c`)

Full TCP state machine including:

- `CLOSED`, `LISTEN`, `SYN_SENT`, `SYN_RECEIVED`, `ESTABLISHED`, `FIN_WAIT_1`, `FIN_WAIT_2`, `CLOSING`, `TIME_WAIT`, `CLOSE_WAIT`, `LAST_ACK`.
- Active `connect()`, passive `listen()`/`accept()`.
- Send/receive with ACK handling and ring buffers.
- Retransmit timer with exponential backoff (10 Hz tick).
- Connect timeout, half-open cleanup, and `TIME_WAIT` expiry.

## Higher-Level Protocols

### DHCP (`dhcp.c`)

Minimal DHCP client following RFC 2131/2132. State machine: `DISCOVER` → `OFFER` → `REQUEST` → `ACK`. Parses options for subnet mask, router, DNS, hostname, lease time, and server ID. Falls back to static configuration from `S:net.conf` if DHCP fails.

### DNS (`dns.c`)

Minimal A-record resolver (RFC 1035). Encodes QNAME labels, handles compression pointers (`0xC0`), and retries with a 2-second timeout per attempt.

### NTP (`ntp.c`)

SNTP client (RFC 4330). Sends a 48-byte request and extracts the Transmit Timestamp. Converts from the NTP epoch (1900) to the Unix epoch (1970) and maintains an epoch counter synchronized against the TSC to avoid RTC interrupt bursts.

### Timezone (`timezone.c`)

Static IANA timezone table with DST rules (month/week/day-of-week/hour). Supports major zones across Australia, New Zealand, Europe, USA, and Asia. The shell `date` and `time` commands use the current zone to display local time.

## Network Device Abstraction

`net/net_device.c` wraps the e1000 and VirtIO-Net drivers behind a common `NetDevice` interface:

- `netdev_register()` / `netdev_init()`
- `netdev_send()` / `netdev_poll()`
- `netdev_get_mac()` / `netdev_set_rx_callback()`
- `netdev_setup_irq()` / `netdev_shutdown()`

The device layer pads Ethernet frames to the minimum 60 bytes and exposes the MAC address to the higher-level stack.

## Drivers

- **Intel e1000 (`kernel/drivers/e1000.c`)**: 82540EM "PRO/1000 MT Desktop" driver. Uses 128 KB MMIO BAR0, legacy TX/RX descriptor rings, and ICR-based IRQ handling.
- **VirtIO-Net (`kernel/drivers/virtio_net.c`)**: Legacy VirtIO network device (PCI vendor `0x1AF4`, device `0x1000`). Uses I/O-port registers and split virtqueues for RX and TX, with INTx support.

## Shell Integration

Network commands in `kernel/shell/` include `netstart`, `netstop`, `ifconfig`, `route`, `ping`, `nslookup`, `ntpd`, and `netinfo` (opens the network info window). Configuration is read from `S:net.conf`.

## Emulated BSD Socket API

M68k Amiga programs can use the network through `bsdsocket.library` (see [bsdsocket.library](/kernel/exec/bsdsocket_library.md)), which maps `socket`, `connect`, `send`, `recv`, etc., to the native TCP/UDP stack.
