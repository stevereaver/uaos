---
name: uaos-debug
description: Debug a running UAOS instance — boot under QEMU, log in remotely via telnetd, and drive the kernel debug suite (klog/dmesg, strace, irqstat, memcheck, chiptrace, crash, taskdump/taskstat/watchdog, pciscan/irqroute, ports/timers/handles/netstat/diskdiag, peek/poke, irqaudit, sercon, tickcheck, etrace, prof, failalloc, pktmon, screenshot) plus host-side tools (serial log, symbolize.sh, analyze_log.py, etrace_decode.py, prof_report.py, gdb_uaos.py, GDB stub, pcap, smoke.sh) and telnet file-exfil via base64/uuencode — which is also how to *see the screen* on real hardware.
argument-hint: "[what to debug]"
allowed-tools:
  - read
  - grep
  - glob
  - exec
---

# UAOS Debugging

This skill covers observing and interacting with a running UAOS system: remote shell via `telnetd`, kernel logging via `klog`/`dmesg`, call tracing via `strace`, the rest of the debug command suite, and the host-side QEMU tooling. Sources of truth: `okf/kernel/klog/index.md`, `okf/kernel/net/index.md` (telnetd section), `kernel/shell/cmd_*.c`, `scripts/`.

## Booting UAOS for a debug session

```bash
bash scripts/build_iso.sh          # produces build/Ultimate_Amiga_OS.iso
bash scripts/run_with_disk.sh      # boots QEMU (virtio-net, serial -> /tmp/uaos_serial.log)
```

`run_with_disk.sh` defaults to `NET=user` (QEMU NAT; guest gets 10.0.2.15 via DHCP). `NET=bridge` uses a TAP device (`sudo bash scripts/net_bridge_setup.sh up` first) and is unstable — prefer NAT.

The stock script captures:
- **Serial log** → `/tmp/uaos_serial.log` (all klog UART output lands here)
- **Packet capture** → `/tmp/uaos_net.pcap` (QEMU `filter-dump`, tcpdump/Wireshark-readable)

### Verify the running build is current (UAOS-283)

Every `make iso` writes a build serial into `s:os-release` — content is
`UAOS-ddmmyyhhmmss` stamped at image-build time. Before trusting any debug
output from a live system, confirm it is running the build you just made:

```bash
# host side — what the ISO carries:
cat build/iso-staging/SYS_ROOT/S/os-release    # or extract from the ISO:
xorriso -osirrox on -indev build/Ultimate_Amiga_OS.iso \
    -extract /SYS_ROOT/S/os-release /tmp/rel.txt && cat /tmp/rel.txt
```

```bash
# guest side (telnet or console shell):
type s:os-release                              # -> e.g. UAOS-071026202817
```

If the serials differ, the target booted stale media (old USB stick, cached
cdrom, `-cdrom` pointing at a previous ISO) — reflash/repoint and reboot
before drawing conclusions from klog/dmesg/strace output. On bare metal with
no telnet, type it in the shell window and read it off the screen (or via
`screenshot` + uuencode exfil).

### Exposing telnetd to the host (NAT mode)

`run_with_disk.sh` has **no hostfwd** — add it to the `-netdev user` line to reach the guest's port 23 from the host:

```
-netdev user,id=n0,net=10.0.2.0/24,host=10.0.2.2,restrict=off,hostfwd=tcp::2323-:23
```

(Verified configuration during the UAOS-45 telnetd work.) With `NET=bridge`, telnet directly to the guest's LAN IP instead.

## telnetd — remote shell login

`C:telnetd` (`kernel/net/telnetd.c`, `kernel/shell/cmd_telnetd.c`) is an **unauthenticated** remote-shell daemon — anyone who reaches the port lands in a shell. Debug facility only.

| Command | Effect |
|---|---|
| `telnetd` | Start daemon on TCP port 23 |
| `telnetd PORT=2323` | Start on a custom port |
| `telnetd STOP` | Stop daemon (kills listener + in-progress session) |

Requirements and behavior:
- The net stack must be up — `netstart` runs automatically in `Startup-Sequence`. `telnetd` refuses with "network stack is down" otherwise.
- **Starting it**: type `telnetd` in the Workbench shell window, or add `C:telnetd` to `system/S/User-Startup` before building so it auto-starts (User-Startup runs after the scheduler is active; putting it in `Startup-Sequence` itself also works but the task only gets its first timeslice later).
- **Connect from host**: `telnet localhost 2323` (NAT + hostfwd) or `telnet <guest-ip> 23` (bridge). `nc` also works — the daemon speaks minimal RFC 854 (`WILL ECHO`, `WILL SGA`, `DO SGA`) and gives per-keystroke echo.
- **One session at a time**: extra connections get a busy banner and clean close (`MAX_REMOTE_SHELLS` = 4 slots but sessions are serialized).
- `endcli` in the remote shell closes the session cleanly; arrow keys and command history work.
- Known issues (tracked under UAOS-47): CR LF produces double newlines, non-arrow CSI sequences leak literal bytes, Ctrl-C collides with the UP vkey, output is not IAC-escaped, no dead-peer/idle timeout.
- The daemon exits on its own when the net stack goes down (`netstop`); after `netstart` a fresh `telnetd` binds cleanly.

## klog — unified kernel logging

`kernel/klog/` is the single canonical logging path. Write path is **ring buffer + UART only** — never VGA/shell/WM — so it is safe to call from IRQ handlers, packet dispatch, and M68k emulator callbacks.

### Levels and subsystems

Levels: `off < err < warn < info < debug < trace` — a message emits when `level <= threshold[subsys]`. Every subsystem defaults to `debug`.

18 subsystems: `kern exec dos vfs net dhcp dns ntp netdev e1000 virtio ide floppy disp audio chip shell strace`

### Shell commands

```
klog                      list all subsystems + current thresholds  (alias: debug)
klog net                  show one subsystem's level
klog dhcp=off             set a threshold
klog vfs,net=debug        comma-list of subsystems
klog all=trace            everything (use sparingly — see warnings)

dmesg                     dump the ring buffer, oldest first
dmesg dhcp dns            filter by subsystem(s)
dmesg warn                filter by minimum level
dmesg dhcp warn           filters combine
dmesg clear               discard all entries
```

Ring buffer: 288 entries × ~168 B ≈ 47 KB; each entry has seq#, subsystem, level, ≤160 B text (overflow truncates). `kprint` boot messages feed the ring via `klog_raw_feed` so `dmesg` shows early boot too.

**Warning**: `klog all=trace` (or `net=trace`-style per-packet logging) can produce hundreds of thousands of blocking 115200-baud UART lines — a past incident starved the PS/2 mouse IRQ and froze the UI for ~20 min. Raise levels only on the subsystem you need; prefer `dmesg` filtering over broad `trace`.

### Kernel-side emit API (for code changes)

```c
KLOG(KLOG_NET, KLOG_DEBUG, "rx packet len=%u", len);   // one-shot, "[net] ..."
klog_puts / klog_putc / klog_appendf                    // streaming fragments
klog_commit()                                           // flush partial line
klog_raw_feed(s) / klog_raw_feedn(s, n)                 // unconditional, ring only
```

New subsystems: add to the enum in `kernel/klog/klog.h` AND `k_subsys_names[]` in `klog.c` together.

## strace — syscall / libcall / packet tracer

`C:strace` (`kernel/shell/cmd_strace.c`) is a Linux-style tracer covering three domains:

1. **x64 INT 0x80 syscalls** — `sys.write`, `sys.open`, `sys.spawn`, `sys.gui_*`, `sys.meminfo`, ... (`sys.schedule` excluded as pure spam)
2. **M68k library calls** — hooked at the real ILLEGAL-trap dispatch (`m68k_illg_instr_callback` in `emulation/uaos_m68k_glue.c`): `exec.*`, `dos.*`, `socket.*` (bsdsocket), `graphics.*`, `intuition.*`, `gadtools.*`
3. **DOS packets** — `ACTION_READ`, `ACTION_WRITE`, `ACTION_LOCATE_OBJECT`, ...

```
strace <command> [args...]     trace a command run
strace -c <cmd>                count calls only; prints count/errors table at end
strace -e <name> <cmd>         filter to one call: "-e sys.open", "-e exec.Wait",
                               "-e OpenLibrary", or bare "-e Wait"
strace -o <file> <cmd>         write trace to a VFS file (e.g. RAM:trace.txt)
strace -t <cmd>                prepend timestamps
```

**Output goes to klog `[strace]` → serial + ring buffer, never the console.** After a run, read it with `dmesg strace`, in `/tmp/uaos_serial.log`, or the `-o` file. Pointer args are never dereferenced (registers only) — safe to run on anything. A re-entrancy guard keeps klog writes from being traced.

Examples:

```
strace dir                    then: dmesg strace
strace -e OpenLibrary run Demos/HelloWorld
strace -c mem                 stats table only
strace -o RAM:trace.txt run <m68k binary>
```

## Full debug command suite

| Command | What it does |
|---|---|
| `klog` / `debug` | Log-level mask control (above) |
| `dmesg` | Ring buffer dump/filter/clear (above) |
| `strace` | Call tracer (above) |
| `irqstat` | Per-vector IRQ counters + rate sampled over 1 s; `irqstat <sec>`, `irqstat NOW` (no wait), `irqstat CLEAR`. First check when a device seems dead. |
| `usbdiag` | Read-only UHCI interrupt-path dump: per controller HC regs (USBSTS/USBINTR, poll-path USBINT latch count, TD IOC), PCI cmd/status/USBLEGSUP (PIRQEN bit13), ICH PIRQ/DxxIR/OIC routing, IO-APIC RTE (mask/trigger/rIRR), LAPIC IRR/ISR. `usbdiag <sec>` samples every stage for 1-60 s while you use the device and prints a verdict on where the interrupt stops. Use when `irqstat` shows a USB vector at 0. |
| `memcheck` | Mungwall-style heap debugging: `memcheck on` adds front/tail guard words to every AllocMem + free-list poisoning; `memcheck` (bare) = status + scan; `memcheck test` = deliberate overwrite self-test; `memcheck dump`; `memcheck off`. Violations report allocating/freeing task names via klog `[memchk]`. |
| `chiptrace` | Custom-chip/CIA access tracer for the AGA/ECS emulator. `chiptrace ON|OFF`, per-class toggles `CHIP`/`CIA`/`PAULA`/`DISK`, `chiptrace PC [N]` samples the M68k PC with disassembly every N ticks, `chiptrace CLEAR`. Output → klog `[chip]`; watch with `dmesg chip`. |
| `crash` | **Deliberate kernel #PF — kills the kernel.** Only for testing the panic dump path + `tools/symbolize.sh`. |
| `taskdump` | Live task table: state, pri, cpu ticks, ctx switches, stack peak/watermark, wait mask, nest levels. `taskdump FULL` or `taskdump <name>` decodes the parked interrupt frame (RIP/CS/RFLAGS/r15..rax) at `native_rsp` — the UAOS-180/181 armed-frame state, no GDB needed. (UAOS-195) |
| `taskstat` | Per-task CPU accounting: `taskstat [<sec>\|NOW]` samples a window and prints cpu%/switches/irqoff/wait — the "hung or spinning" answer. (UAOS-197) |
| `watchdog` | Stall detector armed at boot (`watchdog=<ms>` cmdline, default 5000). Trips when no context switch happens within the budget while ≥2 tasks are runnable, or the tick ISR stops (RTC-second heartbeat). Dumps task states via klog/dbgcon. `watchdog TEST` holds Forbid() past the budget; `watchdog MS=n|OFF`. (UAOS-198) |
| `pciscan` / `irqroute` | PCI enumeration (bdf, class, vendor:dev, pin/line, BARs) + per-device pin→PIRQ→GSI→vector decode with ICH DxxIP/DxxIR handling. `irqroute` flags `vec=UNASSIGNED` / `* UNRESOLVED ROUTE *`. (UAOS-199) |
| `ports` / `timers` / `handles` | Handler MsgPort pending queues + async packet pool; pending TimeRequests (fire tick/delta/sigmask); open-file/lock handle table with owners. (UAOS-200/201/202) |
| `netstat` | TCP socket table (state, addrs, rx/tx depth, retx), UDP sockets, usock layer + owners. (UAOS-203) |
| `diskdiag` | Storage-path stage dump: blockdev registry plus per-driver dumps (IDE status/alt-status + devices, AHCI HBA IS/PI + per-port TFD/SSTS/SERR/CI, virtio-blk queue indices + ISR, virtio-scsi devices, floppy ADF state). `diskdiag TEST=<dev>` times a 1-sector read. (UAOS-204) |
| `peek` / `poke` | Physical/MMIO access (identity-mapped): `peek <addr> [LEN=n] [W=8\|16\|32\|64]`; `poke <addr> <val> FORCE` reports before/write/readback. (UAOS-205) |
| `irqaudit` | Per-task Disable()/Forbid() audit: nest levels, total/max IF=0 time, >50 ms holds, descheduled-in-critical-section count — the UAOS-169/170/176 bug class. (UAOS-206) |
| `sercon` | Two-way serial console: `sercon on|off` (or boot arg `sercon`/`console=ttyS0`). Polled UART RX task with `help ps taskdump dmesg irqstat klog mem tick reboot` — works when IRQ delivery itself is broken. (UAOS-207) |
| `tickcheck` | PIT period vs self-calibrated TSC, IRQ dispatch latency histogram + worst vectors; `tickcheck SEC=n` measures TSC/PIT drift in ppm. (UAOS-208) |
| `etrace` | Binary kernel event ring (ftrace-lite — no UART cost): `etrace MASK=n` (bit0 irq,1 sched,2 signal,3 dos,4 net), `TAIL n` prints recent records, `FILE=path` dumps for `tools/etrace_decode.py`. (UAOS-209) |
| `prof` | PIT-sampled RIP profiler: `prof START|STOP|REPORT [n]|FILE=path`; host-side `tools/prof_report.py` symbolizes. (UAOS-210) |
| `failalloc` | Deterministic alloc-failure injection: `failalloc ON RATE=n|AFTER=n [SEED=n]` — both guest AllocMem and x64 heap. (UAOS-211) |
| `stack` | Now includes per-task peak usage (0xA5 fill watermark) + canary state. (UAOS-212) |
| `pktmon` | In-guest pcap capture: `pktmon START FILE=path [MAX=n]|STOP` — taps netdev TX/RX; works on bare metal where QEMU filter-dump can't. (UAOS-214) |
| `screenshot` | Captures the composited screen to a baseline JPEG: `screenshot` → `RAM:<YYYYMMDDHHMMSS>.jpg` (serial from NTP/RTC time), `screenshot FILE=path [Q=1..100]`. The only way to *see* the display headlessly / on real hardware. (UAOS-218) |
| `mem` | Memory usage summary |
| `ps` | Task list |
| `status FULL` / `status TCB` / `status CLI` | Task/CLI status detail |
| `stack` | Stack size info |
| `libs` | Loaded libraries |
| `jobs` | Background jobs |
| `why` | Last command failure reason |
| `ifconfig` / `route` / `ping` / `nslookup` / `netinfo` | Network state/debug |
| `changetaskpri PRI=n TASK=name` | Reprioritize a task |

All are native shell commands — they work identically in the console window and over telnet.

## Screen capture + file exfil over telnet

`screenshot` (UAOS-218) encodes the whole composited screen to JPEG —
it is the only way to *see* the display when debugging bare metal
(MBP4,1) or a headless QEMU boot. The file lands on `RAM:`; pull it to
the host over the telnet session with either encoder in
`gnu:usr/bin/` (`base64`, `uuencode`/`uudecode` — sharutils tools,
UAOS-217). The same recipe extracts any guest file (pktmon pcaps,
`strace -o` logs, `etrace FILE=` dumps).

### Process A — uuencode (self-framing, recommended)

```bash
# guest side (in a telnet shell):
screenshot                                            # -> RAM:<YYYYMMDDHHMMSS>.jpg
gnu:usr/bin/uuencode RAM:20261002144937.jpg shot.jpg  # prints uuencoded body
```

Capture the session output on the host and `uudecode` it — uudecode
skips the banner/prompt preamble itself and stops at the `end` trailer,
so no manual line filtering is needed:

```bash
{ sleep 1; printf 'screenshot\r'; sleep 8;
  printf 'gnu:usr/bin/uuencode RAM:<file>.jpg shot.jpg\r'; sleep 25;
  printf 'endcli\r'; sleep 1; } | nc -w 60 127.0.0.1 2323 > cap.txt
tr -d '\r' < cap.txt | uudecode   # produces ./shot.jpg (name from header)
```

### Process B — base64

```bash
{ sleep 1; printf 'screenshot\r'; sleep 8;
  printf 'gnu:usr/bin/base64 RAM:<file>.jpg\r'; sleep 25;
  printf 'endcli\r'; sleep 1; } | nc -w 60 127.0.0.1 2323 > cap.txt
# strip CRs + prompt/echo lines, keep only [A-Za-z0-9+/=] body lines:
tr -d '\r' < cap.txt | grep -oE '^[A-Za-z0-9+/=]+$' | base64 -d > shot.jpg
```

Either way, verify locally: `file shot.jpg` → `JPEG image data`, or
`python3 -c "from PIL import Image; Image.open('shot.jpg').load()"`.

### Notes

- Sizing: a 1024×768 Workbench shot is ~70 KB → ~1200 lines of encoded
  text; keep the post-command sleep ≥20 s and the `nc -w` generous.
- telnetd serves one session at a time — run the whole sequence in one
  connection (as above) rather than separate probes.
- The pointer sprite is drawn to VRAM at flip time and is NOT captured.
- Remote shell runs the same native table, so `screenshot` works from
  telnet exactly as in the console window.

## Host-side tools

- **Serial log**: `/tmp/uaos_serial.log` — everything klog writes to UART. `tail -f` it during a session. It is output-only; you cannot type into the guest over serial (interaction is GUI window or telnet only).
- **`tools/symbolize.sh`**: resolves hex addresses from a panic dump/serial log to `function+offset` and `file:line` (via addr2line when built with `-g`). `tools/symbolize.sh /tmp/uaos_serial.log` or paste addresses as args.
- **`scripts/debug_qemu.sh`**: boots QEMU with `-s -S` — GDB stub on `tcp::1234` (`GDB_PORT=` override), CPU halted until continue. Attach:
  ```
  gdb build/uaos-kernel.elf
  (gdb) target remote :1234
  (gdb) hbreak uaos_kernel_main
  (gdb) continue
  ```
  Kernel is on a 4 GB identity map (VA = PA), so ELF symbols resolve directly. Use `hbreak` (hardware breakpoints) for code in read-only pages.
- **pcap**: `/tmp/uaos_net.pcap` — open in Wireshark/tcpdump to debug net stack issues at the wire level.
- **`tools/analyze_log.py`** (UAOS-216): serial-log analyzer — panic extraction + RIP symbolization, warn/err rollup, watchdog events. `tools/analyze_log.py /tmp/uaos_serial.log`.
- **`tools/etrace_decode.py`** (UAOS-209): decodes `etrace FILE=` dumps (`ETRC` magic, 24 B records). `--elf` symbolizes, `--hz` adds µs deltas.
- **`tools/prof_report.py`** (UAOS-210): `prof FILE=` rows → sorted symbolized hotspot table.
- **`tools/gdb_uaos.py`** (UAOS-213): `source` it inside the GDB-stub session for `uaos tasks|task NAME|timers|stack NAME` — walks `g_tasks[]` via DWARF, decodes parked frames.
- **`tests/smoke.sh`** (UAOS-215): headless QEMU + telnet command battery — asserts each debug command produces output, archives serial log + pcap to `build/smoke-<ts>/`.

## Standard debug workflow

1. `bash scripts/build_iso.sh` (add `-g`-friendly flags only if you need DWARF for symbolize/GDB line info).
2. Boot with `scripts/run_with_disk.sh` — add `hostfwd=tcp::2323-:23` to the `-netdev user` args first if you want host telnet (or use `debug_qemu.sh` when you need GDB).
3. In the Workbench shell: `telnetd` (or pre-seed `C:telnetd` in `system/S/User-Startup`).
4. `telnet localhost 2323` from the host → remote `RAM:>` shell.
5. `type s:os-release` and compare with `cat build/iso-staging/SYS_ROOT/S/os-release` — mismatch means stale media; fix the boot target before proceeding (see "Verify the running build is current" above).
6. Drive the investigation: `klog <subsys>=trace` for the suspect subsystem, reproduce, then `dmesg <subsys>` / read `/tmp/uaos_serial.log`. Use `strace` for call-level visibility and `irqstat`/`memcheck`/`chiptrace` for IRQ, heap, and chipset problems.
7. On a panic: capture the dump from the serial log and run `tools/symbolize.sh` on it.

## Gotchas

- The serial device is write-only from the host side (`-serial file:`) — no input channel for sercon in the stock script. To exercise `sercon`, change `-serial` to `-serial tcp::4444,server,nowait` (then `nc localhost 4444`) or a pty; with `file:` the console still logs normally but guest RX sees nothing.
- `telnetd` serves one session; if a client wedges the session, `telnetd STOP` frees it.
- `dmesg` reads the 288-entry ring — long traces overflow; for big captures use `strace -o RAM:file`, `etrace FILE=RAM:x` (binary ring — no UART cost), or read the serial log on the host.
- klog/strace/chiptrace output deliberately bypasses the console — if you "see nothing", check `dmesg` or the serial log, not the shell window.
- `etrace MASK=0x1f` traces at IRQ/sched rate with ~zero overhead — use it instead of `klog all=trace` when chasing timing bugs; the UART starvation incident is exactly what it avoids.
- `watchdog` is armed by default (5 s). On bare metal a trip paints the task dump via dbgcon even with no serial — read it off the screen or the ring.
- `crash`, `poke ... FORCE`, and `format`/`install`-class commands are destructive — `poke` to an MMIO register can wedge the bus.
- Boot args for diagnostics: `watchdog=<ms>` (budget, 0=off), `sercon` or `console=ttyS0` (serial console at boot).
