---
type: Kernel Subsystem
title: TCP/IP Network Stack
description: The native IPv4 networking stack, device drivers, and higher-level protocols in UAOS.
resource: /kernel/net/
tags: [network, tcp, udp, ip, dhcp, dns, ntp]
timestamp: 2026-10-05T06:00:00Z
---

# TCP/IP Network Stack

UAOS includes a native IPv4 networking stack implemented in `kernel/net/`. It supports ARP, ICMP, UDP, TCP, DHCP, DNS, and NTP, and is exposed to emulated M68k programs through `bsdsocket.library`.

## Initialization

`net/stack.c` provides the top-level API. At boot (or on `C:NetStart`), the stack auto-probes for a network device (`netdev_probe()` in `net_device.c`):

1. Try Marvell sky2 (`kernel/drivers/sky2.c`) — the MacBookPro4,1's real NIC.
2. If no sky2 is found, try Intel e1000 (`kernel/drivers/e1000.c`).
3. If no e1000 is found, try VirtIO-Net (`kernel/drivers/virtio_net.c`).

Once a device is registered through `netdev_register()`, the stack can send and receive Ethernet frames.

## Protocol Layers

### Ethernet (`eth.c`)

Simple RX dispatch: ARP frames (`0x0806`) go to the ARP handler; IPv4 frames (`0x0800`) go to the IP layer.

### ARP (`arp.c`)

Maintains a 16-entry ARP cache (`ARP_CACHE_SIZE`) with true LRU eviction — each entry carries a `last_use` stamp updated on insert, refresh, and lookup hit, and a full cache evicts the oldest non-pinned slot. The default gateway's entry is pinned (`arp_set_gateway`, called from `ip_init`) so chatty LAN hosts can never evict it. Cache learning follows RFC 826: `arp_rx` refreshes the sender's MAC only if an entry already exists, and creates new entries only for packets that target our IP (UAOS-166 — previously every broadcast request was learned, which filled the cache with random LAN hosts and the always-evict-slot-0 policy kicked out the gateway). Sends ARP requests and replies, and calls `ip_arp_resolved()` when a learn/refresh may unblock queued TX frames. Used by the IP layer before sending to a non-local address.

### IPv4 (`ip.c`)

- Verifies header checksums.
- Drops fragmented packets (no reassembly).
- Handles local delivery, broadcast, and gateway routing.
- Dispatches to ICMP, UDP, or TCP based on the protocol field.
- ARP-miss pending queue (UAOS-167): a send whose next-hop is unresolved
  used to drop the packet outright (the first DNS query to the gateway
  was always lost, costing the full ~2 s retry).  `ip_send` now queues
  up to `ARP_PEND_MAX` (8) complete frames — at most `ARP_PEND_PER_PEER`
  (2) per next-hop, each with an `ARP_PEND_TTL` (~3 s) — and
  `arp_rx` → `ip_arp_resolved()` fills in the Ethernet header and
  transmits them when the reply lands.  ARP requests are throttled to
  one per `ARP_REQ_MIN_GAP` (~1 s) per next-hop.
- Serial debug output is limited to errors only (bad length, bad checksum, fragments). Per-packet "rx proto" and "dispatching" logging was removed because it produced ~180k lines of blocking serial output at 115200 baud (~20 min of CPU time), which starved the PS/2 mouse IRQ (IRQ 12, lower priority than E1000's IRQ 11 on the slave 8259A PIC) and froze the UI.

### ICMP (`icmp.c`)

Implements echo request/reply (ping). The `ping` shell command uses this layer and waits for `icmp_got_reply()`.

### UDP (`udp.c`)

- Socket table with ephemeral port allocation (`49152`–`65535`). Ports are drawn at random from the kernel entropy source (UAOS-168) with a sequential-counter fallback, so DNS/NTP source ports aren't predictable.
- Datagram-oriented RX path (UAOS-185): each socket keeps a FIFO of up to `UDP_RX_QUEUE` (8) `{src_ip, src_port, len}` records whose payloads occupy contiguous spans of the 2048-byte `rx_buf` ring.  `udp_recv` pops exactly one datagram per call and returns that datagram's real peer — BSD `recvfrom` semantics — and an undersized buffer truncates the datagram (unread tail discarded).  A datagram that doesn't fit the queue or ring whole is dropped whole; a partial record would corrupt FIFO accounting.  Queue/payload operations are `irq_save`-guarded because the RX poll path and socket readers run on different preemptable tasks.  (Previously a flat byte ring merged all datagrams into a stream attributed to the last sender.)
- Used by DHCP, DNS, and NTP.

### TCP (`tcp.c`)

Full TCP state machine including:

- `CLOSED`, `LISTEN`, `SYN_SENT`, `SYN_RECEIVED`, `ESTABLISHED`, `FIN_WAIT_1`, `FIN_WAIT_2`, `CLOSING`, `TIME_WAIT`, `CLOSE_WAIT`, `LAST_ACK`.
- Active `connect()`, passive `listen()`/`accept()`.  `tcp_accept`
  returns only unclaimed sockets on the listen port — `ESTABLISHED` or
  `CLOSE_WAIT` (UAOS-223: a peer that FINs between handshake and accept
  must still reach a reader, which drains the ring and closes, instead
  of orphaning the slot) — and marks each with `accepted` on the way
  out — without the mark, a live session socket (also `ESTABLISHED` on
  that port) would be handed out again to the next caller.  Outbound
  `tcp_connect` sockets are born `accepted` so they can never be
  mistaken for pending accepts.
- Send/receive with ACK handling and ring buffers.
- Retransmit timer with exponential backoff (`tcp_tick` runs at the
  100 Hz PIT rate via `net_stack_tick()` in `PIT_IRQHandler`).
- Connect timeout, half-open cleanup, `TIME_WAIT` expiry, and teardown
  bounds (UAOS-223): an incoming RST now closes the socket in **every**
  synchronized state (previously honored only in `SYN_SENT`/
  `ESTABLISHED`, so a force-closing peer's RST/RST|ACK was ignored in
  `CLOSE_WAIT`, `LAST_ACK`, `FIN_WAIT_*`, `SYN_RECEIVED` and the slot
  leaked).  `CLOSE_WAIT` carries a 30 s idle linger bound — real owner
  activity (`tcp_send`/`tcp_recv` progress) refreshes `conn_timer`, so
  only abandoned sockets are reaped, by driving the normal close path.
  `FIN_WAIT_2` is bounded at 120 s (peer data refreshes it) — a peer
  that never sends FIN can no longer pin a slot.  `snd_una` no longer
  disarms the retransmit timer on a *partial* ACK (the saved segment
  and a fresh RTO are kept), and `tcp_tick` re-arms the RTO whenever
  data is unacked but the timer is dead — so a `fin_pending` deferred
  FIN can never wedge a socket again.  `FIN_WAIT_1`→`FIN_WAIT_2` and
  `LAST_ACK`→`CLOSED` now require an ACK covering our FIN, so a stale
  dup ACK can neither skip the FIN's retransmissions nor close the
  socket early.
- Duplicate-SYN handling: a retransmitted SYN matching a `SYN_RECEIVED`
  socket replays the saved SYN-ACK segment.  `ip_send` no longer drops
  on an ARP miss (see the ARP-miss pending queue under IPv4), but real
  loss still happens — without the replay a half-open connection whose
  SYN-ACK was lost could never complete.  `SYN_RECEIVED` sockets also
  have a `conn_timer` timeout so dead half-opens do not leak socket
  slots.
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

Minimal A-record resolver (RFC 1035). Encodes QNAME labels, handles compression pointers (`0xC0`), and retries with a 2-second timeout per attempt — but only on real timeouts: `dns_parse_response` returns a tri-state so any definitive answer (nonzero RCODE like NXDOMAIN/SERVFAIL/REFUSED, a NODATA empty answer, or a response with no usable A record) ends the retry loop immediately. `localhost` (any case, optional trailing dot) short-circuits to 127.0.0.1 with no query at all, even with no DNS server configured.

Spoof-resistance (UAOS-168): the transaction ID comes from `entropy_fill` (the old hostname-derived ID was deterministic), the UDP source port is random (see `alloc_port` in `udp.c`), and a response is only accepted if its source IP/port match the queried server and its question section echoes our query byte-for-byte. With no caller `poll_fn`, the wait blocks on `SIGF_NET` via `Task_WaitTicks` (armed by `net_rx_notify_arm`) instead of a busy-spin.

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

- **Marvell sky2 (`kernel/drivers/sky2.c`, UAOS-137)**: Yukon-2 driver (88E8058 "EC Ultra" in the MacBookPro4,1, plus the wider Yukon-2 device-ID family). 16 KB MMIO BAR0 with the PCI config window mapped at BAR0+0x1C00; RAM-based list-element architecture — TX/RX rings of 8-byte LEs fed to the prefetch units, completions reported via a shared status ring (OP_RXSTAT / OP_TXINDEXLE). Rings must be 32 KB aligned: the prefetch/status base registers drop address bits [11:0]. Synchronous TX (`sky2_send` drains the status ring until the TX index passes its LE, frame staged in a per-LE bounce buffer); the drain is cli-protected and only *records* completions — RX frames are delivered from `sky2_poll` task context via an atomic pop+copy+resubmit so IRQ/send/poll can never re-enter the net stack. IRQ: MSI first, INTx fallback; `B0_IMSK` gates status-BMU writeback on this silicon so it is unmasked even in poll mode, and `B0_Y2_SP_ISRC2` must never be read outside the handler (the read masks device IRQs until `B0_Y2_SP_LISR`). **Metal-verified** on the MacBook: MSI vec 97, 1000baseT FD, DHCP/DNS/NTP/ping/telnetd all live, concurrent remote sessions OK.
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
  consumes sub-negotiations (`SB ... SE`), answers `DO`/`DONT`/`WILL`/
  `WONT` per RFC 854 (`DO` for unoffered options gets `WONT`, `WILL`
  gets `DONT`), maps `IAC IAC` to a literal `0xFF`, answers `IAC AYT`
  with `[UAOS yes]`, and treats `IAC IP`/`IAC AO` as a break request.
  `CR LF`, `CR NUL` and bare `CR` all collapse to a single line-feed —
  the `NVT_CR` state swallows the byte following a `CR` when it is
  `LF`/`NUL`, so one Enter can never dispatch a phantom empty command
  (UAOS-48).
- Escape sequences are parsed as full CSI/SS3, not byte-at-a-time: CSI
  parameter (`0x30-0x3F`) and intermediate (`0x20-0x2F`) bytes are
  consumed until a final byte (`0x40-0x7E`) ends the sequence, so
  `ESC [ 3 ~`, `ESC [ 1 ; 5 A` and friends can no longer leak literal
  bytes into the input line (UAOS-49).  Arrow finals map to the shell's
  virtual cursor keys; `H`/`F`, `1~`/`4~`/`7~`/`8~` and `3~` map to
  Home/End/Delete line-editor behavior; `ESC O` SS3 arrows and
  Home/End work too.
- Outbound data is IAC-escaped per RFC 854: `remote_send()` in
  `shell_win.c` doubles every literal `0xFF` byte so a strict Telnet
  client never mistakes shell output for protocol bytes (UAOS-51).
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
waits for `g_running` and `g_pump_count` to drain by calling
`Task_Yield()` — a real reschedule since UAOS-169, so the daemon runs
immediately — with an `irq_save`-wrapped `sti;hlt` fallback when no
switch is possible (UAOS-176: the `sti` is needed for the `hlt` to wake
on IRQs, but the caller's IF is restored afterwards) and
a ~1 s `g_pit_ticks` deadline;
in contexts where the scheduler cannot preempt — Startup-Sequence
executes in kernel-main context before `Task_StartFirst()`, and `&`
background jobs run under the idle task's `Forbid()` (which `Task_Yield`
honours) — it simply times out and the daemon exits on its first
timeslice.  The daemon also exits on its own when the net stack goes
down: `net_stack_shutdown()` does not tear down TCP sockets, so the task
watches `net_stack_is_up()` rather than the listener's `tcp_state()`.
After `netstart` a fresh `telnetd` binds cleanly.  `Telnetd_Start()`
rolls `g_running` back if `Task_CreateNative()` fails so a failed spawn
cannot leave the service permanently "running".

`tcp_abort(sock)` (tcp.c) is the non-transmitting counterpart to
`tcp_close()`: it forces the socket to `TCP_CLOSED` unconditionally,
for teardown when the peer can no longer be reached.

Session watchdog and logging (UAOS-60/61): each pump tracks `last_rx`
(last received input).  At ~5 min idle (`TELNETD_IDLE_PROBE_TICKS`) it
sends `IAC AYT` as a liveness probe; at ~10 min
(`TELNETD_IDLE_TICKS`) it closes the socket and kills the shell via
`ShellWin_RemoteKill()`, so a peer that vanishes without FIN/RST can no
longer pin a remote slot forever.  Connects and disconnects are logged
to klog/serial with peer IP:port and session duration, and `telnetd
STATUS` lists live sessions.  Because the daemon is unauthenticated,
remote sessions are also policy-guarded at the shell's `run_cmd` choke
point — framebuffer/desktop-only commands (`calculator`, `loadwb`, the
prefs editors, ...) are refused, and destructive commands (`delete`,
`format`, `reboot`, ...) are logged to klog/serial; see the Remote
Shell Sessions section of [Display](/kernel/display/index.md)
(UAOS-59).

Earlier audit fixes that already landed: concurrency (per-session pump
tasks, tokenized handles, busy banner) in UAOS-53, remote interrupt
handling (Ctrl-C / IAC IP / IAC AO → real shell break) in UAOS-50, TCP
RX-overflow dropped-but-ACKed in UAOS-56, and peer-window enforcement,
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

## Userspace Socket Layer (`usock.c`, UAOS-143)

Native x86-64 tasks reach the same TCP/DNS stack through the
`SYSCALL_NET_*` syscall block (`0x38`–`0x3F`, dispatched in
`syscall_dispatch.c` to `kernel/net/usock.c`):

- `NET_SOCKET`/`NET_CLOSE` (0x38/0x3C) — allocate/release a handle from a
  per-system table (`MAX_USOCKS` = 8).  Handles are stamped with the
  creating task; `usock_cleanup_task()` runs from `Task_Exit` so a killed
  or crashing tool cannot leak native sockets.  Sockets are closed
  gracefully (`tcp_close`) while the stack is up and aborted
  (`tcp_abort`) when it is down.
- `NET_CONNECT` (0x39) — wraps `tcp_connect()` and polls
  `net_stack_poll()` + `Task_SleepTicks(1)` until `ESTABLISHED`,
  `CLOSED` (RST/timeout), or the per-socket deadline.  The kernel
  `tcp_tick` half-open reaper bounds the wait regardless.
- `NET_SEND`/`NET_RECV` (0x3A/0x3B) — block-with-deadline wrappers that
  hide the kernel's non-blocking primitives: `tcp_send` returns 0 while
  the single in-flight segment is unacked or the peer window is closed,
  and `tcp_recv` returns 0 while the RX ring is empty, so usock polls
  and sleeps in tick quanta instead of busy-spinning.  EOF (peer
  FIN + drained ring) returns 0 to the caller.
- `NET_SETOPT` (0x3E) — connect/recv/send deadlines in ms
  (0 = wait forever; defaults 8 s connect / 30 s recv/send).
- `NET_STATE` (0x3F) — expose the native `TcpState` for diagnostics.
- `NET_RESOLVE` (0x3D) — hostname → IPv4 via `dns_resolve()` with a
  cooperative poll callback (`net_stack_poll` + sleep slice), so the
  resolver's internal ~50 ms steps share the CPU instead of spinning.

The userspace-facing mirror lives in `system/libuaos/uaos_socket.h`
(header-only inline wrappers, `UAOS_SOCK_STREAM`, `UAOS_E*` codes).

Two `tcp.c` behaviours were fixed in service of this layer (UAOS-143):

- **Ephemeral port rotation.**  `tcp_connect` used to derive the local
  port from the socket slot index (`49152 + idx`), so every sequential
  connection reused the same 4-tuple.  The guest's TIME_WAIT is only
  2 s (`TCP_TIMEWAIT_TICKS`) while a real host holds ~60 s — a fast
  reconnect (e.g. an HTTP redirect chain) was answered with RST.
  `pick_ephemeral_port()` now rotates 49152–65535 and skips ports held
  by live sockets.
- **Window-update on drain.**  `tcp_recv` only emitted a window-update
  ACK when the ring had been *completely* full; near-full drains left
  the peer in zero-window persist mode, stalling transfers ~2 s per
  probe cycle (~1.8 KiB/s).  The ACK now goes out whenever a read frees
  space in a non-empty ring — a 128 KiB download completes in ~1.25 s.

### TLS (BearSSL, UAOS-147)

TLS 1.2 terminates entirely in userspace: the kernel only sees TCP.
`system/libuaos/uaos_tls.h` drives a `br_ssl_client_context` +
`br_x509_minimal_context` over the `uaos_socket.h` transport, using
BearSSL's bidirectional engine buffer.  The BearSSL 0.6 subset needed
by a client link (~92 sources) is vendored under `system/bearssl/` and
compiled freestanding — only `compat/string.h`/`compat/time.h` shims
are required, plus a tiny `mem*`/`time()` shim translation unit.
Trust anchors live in `system/bearssl/ta_roots.c` (generated by
`tools/gen_ta_roots.py` from the host's Mozilla CA bundle); entropy is
`SYSCALL_GETRANDOM` (0x41, `kernel/drivers/entropy.c`: RDRAND/RDSEED
via CPUID, RDTSC-jitter fallback) injected before
`br_ssl_client_reset()`, and cert-validity time is `SYSCALL_TIME`
(0x40, `ntp_get_epoch()`).

Two engine semantics worth remembering:

- `br_ssl_engine_current_state` is computed from buffer pointers, not
  a phase enum: post-handshake the client normally sits in
  `SENDAPP|RECVREC` (0x0C) — the app channel is open and the engine is
  merely *offering* inbound buffer space.  Pumping must not treat a
  pending `RECVREC` as mandatory once `SENDAPP` is available, or the
  client deadlocks waiting for a record the server will never send.
- `br_ssl_engine_sendapp_ack()` only emits the record when the buffer
  fills — call `br_ssl_engine_flush()` or short writes (an HTTP
  request) never reach the wire.
