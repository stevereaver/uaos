---
type: Kernel Subsystem
title: TCP/IP Network Stack
description: The native IPv4 networking stack, device drivers, and higher-level protocols in UAOS.
resource: /kernel/net/
tags: [network, tcp, udp, ip, dhcp, dns, ntp]
timestamp: 2026-09-25T01:20:00Z
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
- Active `connect()`, passive `listen()`/`accept()`.  `tcp_accept`
  returns only unclaimed `ESTABLISHED` sockets on the listen port and
  marks each with `accepted` on the way out — without the mark, a live
  session socket (also `ESTABLISHED` on that port) would be handed out
  again to the next caller.  Outbound `tcp_connect` sockets are born
  `accepted` so they can never be mistaken for pending accepts.
- Send/receive with ACK handling and ring buffers.
- Retransmit timer with exponential backoff (10 Hz tick).
- Connect timeout, half-open cleanup, and `TIME_WAIT` expiry.
- Duplicate-SYN handling: a retransmitted SYN matching a `SYN_RECEIVED`
  socket replays the saved SYN-ACK segment.  This matters because the
  first SYN-ACK is dropped by `ip_send` whenever the ARP cache is cold
  (the packet is discarded while the ARP request resolves); without the
  replay the half-open connection could never complete.  `SYN_RECEIVED`
  sockets also have a `conn_timer` timeout so dead half-opens do not
  leak socket slots.
- Receive flow control: every outgoing segment advertises the RX ring's
  actual free space (0-window when full).  `tcp_rx_data` accepts only
  in-order bytes — retransmit overlap is trimmed, a segment ahead of
  `rcv_nxt` is dropped with a dup-ACK — and `rcv_nxt` advances only by
  bytes actually queued, so a full ring drop-and-NACKs the tail for the
  peer to retransmit instead of silently losing it (UAOS-56).  A FIN is
  consumed only once all preceding data has been delivered.  Data is
  accepted in `ESTABLISHED`, `FIN_WAIT_1`, `FIN_WAIT_2` (half-close) and
  `CLOSE_WAIT` (pre-FIN retransmits; dup FINs are re-ACKed).  `tcp_recv`
  pushes a window-update ACK when draining reopens a previously full
  ring, so the peer does not sit out its zero-window persist backoff.
- Send flow control (UAOS-55): `tcp_send` enforces the peer's advertised
  receive window (`snd_wnd`) and allows only **one seq-carrying segment
  in flight** per socket — `retx_buf` holds a single segment, so a second
  send while the first is unacked would leave a hole retransmit could
  never fill.  It returns 0 when busy or when the window is closed, and
  callers (`remote_send`, `bsd_send`, telnetd `send_buf`) poll the stack
  and retry.  `tcp_close` defers its FIN (`fin_pending`) while data is
  unacked so the FIN cannot clobber the outstanding segment's retx
  state; `tcp_tick` releases it once `snd_una` catches up.  `snd_una`
  only advances on ACKs inside `(snd_una, snd_nxt]` — stale/reordered or
  out-of-range ACKs can no longer rewind it — while `snd_wnd` is still
  taken from any ACK (dup ACKs carry fresh window information, e.g. a
  reopened zero window).  Segments that match no socket get an RFC 793
  RST (`tcp_send_reset`) so closed ports refuse connections instead of
  silently dropping; RSTs are never answered with RST, and only segments
  actually addressed to the local IP are answered.

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
- **VirtIO-Net (`kernel/drivers/virtio_net.c`)**: VirtIO network device supporting both transports: legacy/transitional `1af4:1000` (BAR0 I/O-port registers) and modern non-transitional `1af4:1041` (virtio-1.0 vendor-capability transport — common config, notify, ISR and device-config regions; `VIRTIO_F_VERSION_1` + `VIRTIO_NET_F_MAC` negotiated, 12-byte `virtio_net_hdr`). Modern regions are accessed by MMIO dereference when the BAR maps inside the identity-mapped 4 GB, or via the `VIRTIO_PCI_CAP_PCI_CFG` config-space window when firmware places the BAR above 4 GB (OVMF on q35 puts the 64-bit BAR at ~768 GB). Split virtqueues for RX and TX, with INTx support. The legacy `QUEUE_SIZE` register is read-only, so the driver honours the device-reported queue size when laying out rings (QEMU = 256, VirtualBox = 1024; hardcoding 256 placed the avail/used rings at wrong offsets under VirtualBox and broke all TX/RX). Up to 1024-entry queues are supported; at most 256 RX buffers are posted.

## Shell Integration

Network commands in `kernel/shell/` include `netstart`, `netstop`, `ifconfig`, `route`, `ping`, `nslookup`, `ntpd`, `netinfo` (opens the network info window), and `telnetd`. Configuration is read from `S:net.conf`.

## Telnet Daemon (`telnetd.c`)

`kernel/net/telnetd.c` implements an unauthenticated remote-shell service
(`C:telnetd`, default TCP port 23, `PORT=` override).  A dedicated
`telnetd` task runs `tcp_listen()`/`tcp_accept()`; each accepted socket is
bridged to a remote `ShellInstance` (see the "Remote Shell Sessions"
section of [Display](/kernel/display/index.md)).  It is a debugging
facility — anyone who can reach the port lands directly in a shell with
no login.

Telnet protocol handling is deliberately small:

- On connect the daemon sends `WILL ECHO`, `WILL SGA`, `DO SGA`
  (server-echo, character-at-a-time mode) as **one** 9-byte segment.
  Sending each option separately risked tearing: `tcp_send` accepts only
  one in-flight segment, so `WILL SGA` could be dropped when `WILL ECHO`
  was still unacked — leaving the client in line mode with local echo
  off and typed keys invisible until Enter.
- An NVT state machine in the pump strips `IAC` command sequences,
  consumes sub-negotiations (`SB ... SE`), refuses `DO`/`WILL` for
  options it did not offer, maps `IAC IAC` to a literal `0xFF`, and
  translates `CR`, `CR NUL`, and `LF` to a single line-feed for the shell.
- `ESC [` / `ESC O` final bytes `A`/`B`/`C`/`D` are mapped to the shell's
  virtual cursor-key codes so command history works over the wire.
- A raw `0x03` byte (Ctrl-C) and the Telnet `IAC IP` / `IAC AO` commands
  are all fed to the shell as a break request (UAOS-50).  They are no
  longer mapped to backspace or cursor keys: the shell's virtual-key
  codes moved out of the ASCII control range, so ETX reaches the
  session as a real interrupt and cannot be misread as `SHELL_VKEY_UP`
  (which previously recalled history and could re-execute a command).

Sessions are served concurrently (UAOS-53): the `telnetd` listener task
only accepts — each accepted socket is handed to its own
`telnetd-session` pump task so `tcp_accept()` keeps running while
sessions are active.  Up to `MAX_REMOTE_SHELLS` (4) remote shells can be
live at once; a connection arriving when no slot (or pump context) is
free gets a "no remote shell slots free" banner and a clean close
instead of completing the handshake onto a silent socket.

Each pump task calls `tcp_recv()`, `net_stack_poll()`, and `Task_Yield()`
in a loop, exits when the socket leaves `ESTABLISHED`/`CLOSE_WAIT`, when
the peer half-closes with a drained RX buffer, when the shell session
ends (`endcli`), when the daemon is stopping (its `PumpCtx.gen` no
longer matches `g_generation`), or when the net stack goes down.  On
exit it sends a closing banner and calls `tcp_close()` — unless the
stack is already down, in which case it calls `tcp_abort()` instead (a
FIN could never be answered, and `tcp_tick` no longer runs to retire
the socket, so a graceful close would leak the slot in `FIN_WAIT_1`).
`g_pump_count` tracks live pumps so `Telnetd_Stop()` can wait for them;
the counter updates are cli/sti-guarded because `++` in the listener and
`--` in pump tasks would otherwise race.

Remote session handles are tokenized: `ShellWin_RemoteOpen()` stamps a
monotonically increasing token into the opaque handle alongside the slot
index, so a pump task can never feed input to a different session that
later reuses the same remote slot (see the "Remote Shell Sessions"
section of [Display](/kernel/display/index.md)).

Lifecycle (UAOS-54): `telnetd STOP` maps to `Telnetd_Stop()`, which sets
a `g_stop` flag the daemon checks every loop iteration — the accept loop
and any in-progress session pump both unwind, the listener is
`tcp_close()`d, `g_running` clears, and the task exits.  `Telnetd_Stop`
waits for `g_running` and `g_pump_count` to drain by polling on the 100
Hz PIT tick with a ~1 s deadline (Task_Yield is a bare `pause` and
Wait() has no timeout);
the wait relies on timer preemption, so in contexts where the scheduler
cannot run — Startup-Sequence executes in kernel-main context before
`Task_StartFirst()`, and `&` background jobs run under the idle task's
`Forbid()` — it simply times out and the daemon exits on its first
timeslice.  The daemon also exits on its own when the net stack goes
down: `net_stack_shutdown()` does not tear down TCP sockets, so the task
watches `net_stack_is_up()` rather than the listener's `tcp_state()`.
After `netstart` a fresh `telnetd` binds cleanly.  `Telnetd_Start()`
rolls `g_running` back if `Task_CreateNative()` fails so a failed spawn
cannot leave the service permanently "running".

`tcp_abort(sock)` (tcp.c) is the non-transmitting counterpart to
`tcp_close()`: it forces the socket to `TCP_CLOSED` unconditionally,
for teardown when the peer can no longer be reached.

Known issues (live audit, 2026-09-25 — tracked under UAOS-47; updated
after UAOS-50/53): CR LF produces two newlines; non-arrow CSI sequences
leak literal bytes; output is not IAC-escaped; no dead-peer/idle
timeout.  Concurrency (per-session pump tasks, tokenized handles, busy
banner) landed in UAOS-53 and remote interrupt handling (Ctrl-C /
IAC IP / IAC AO → real shell break) in UAOS-50.  The TCP-layer gaps that hit telnetd directly were fixed:
RX overflow dropped-but-ACKed in UAOS-56, and peer-window enforcement,
single-segment-in-flight send, `snd_una` validation, and RST generation
in UAOS-55.

## VirtIO-Net TX Serialization

`virtio_net_send()` uses a single TX descriptor (slot 0) over a shared
`g_tx_hdr_buf`.  Because the scheduler is preemptive and the NIC IRQ
handler can also inject sends (TCP ACKs from `tcp_rx` inside
`virtio_net_poll`), the function wraps the whole claim/fill/notify
sequence in `Disable()`/`Enable()` (cli/sti with nesting).  Without this,
concurrent senders overwrote each other's frames mid-flight; corrupted
segments failed the peer's checksum, left sequence holes the
single-segment TCP retransmit model cannot fill, and stalled
connections.  Found and verified during the telnetd work.

Before overwriting the shared buffer the send path waits for the
previous submission to be *outstanding-consumed*: it spins while
`avail->idx - used->idx != 0` (bounded, ~200k pauses).  Device models
differ here: VirtualBox posts TX used entries promptly, while QEMU
consumes the avail ring without posting used entries, so a full
timeout with no used-ring progress latches `g_tx_no_used` and the wait
is skipped thereafter.  The earlier check (`last_used != used->idx`)
was inverted — it spun while the device had *already returned*
descriptors and could never clear — which made every send after the
first burn the entire spin bound with IRQs disabled under VirtualBox,
freezing the machine on the first telnet burst.  `virtio_net_poll()`
also only rings the RX doorbell when it actually re-added descriptors;
the doorbell write is a VM exit and callers poll in tight loops.

## Emulated BSD Socket API

M68k Amiga programs can use the network through `bsdsocket.library` (see [bsdsocket.library](/kernel/exec/bsdsocket_library.md)), which maps `socket`, `connect`, `send`, `recv`, etc., to the native TCP/UDP stack.
