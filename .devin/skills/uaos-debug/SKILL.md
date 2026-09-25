---
name: uaos-debug
description: Debug a running UAOS instance — boot under QEMU, log in remotely via telnetd, and drive the kernel debug suite (klog/dmesg, strace, irqstat, memcheck, chiptrace, crash) plus host-side tools (serial log, symbolize.sh, GDB stub, pcap).
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
| `memcheck` | Mungwall-style heap debugging: `memcheck on` adds front/tail guard words to every AllocMem + free-list poisoning; `memcheck` (bare) = status + scan; `memcheck test` = deliberate overwrite self-test; `memcheck dump`; `memcheck off`. Violations report allocating/freeing task names via klog `[memchk]`. |
| `chiptrace` | Custom-chip/CIA access tracer for the AGA/ECS emulator. `chiptrace ON|OFF`, per-class toggles `CHIP`/`CIA`/`PAULA`/`DISK`, `chiptrace PC [N]` samples the M68k PC with disassembly every N ticks, `chiptrace CLEAR`. Output → klog `[chip]`; watch with `dmesg chip`. |
| `crash` | **Deliberate kernel #PF — kills the kernel.** Only for testing the panic dump path + `tools/symbolize.sh`. |
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

## Standard debug workflow

1. `bash scripts/build_iso.sh` (add `-g`-friendly flags only if you need DWARF for symbolize/GDB line info).
2. Boot with `scripts/run_with_disk.sh` — add `hostfwd=tcp::2323-:23` to the `-netdev user` args first if you want host telnet (or use `debug_qemu.sh` when you need GDB).
3. In the Workbench shell: `telnetd` (or pre-seed `C:telnetd` in `system/S/User-Startup`).
4. `telnet localhost 2323` from the host → remote `RAM:>` shell.
5. Drive the investigation: `klog <subsys>=trace` for the suspect subsystem, reproduce, then `dmesg <subsys>` / read `/tmp/uaos_serial.log`. Use `strace` for call-level visibility and `irqstat`/`memcheck`/`chiptrace` for IRQ, heap, and chipset problems.
6. On a panic: capture the dump from the serial log and run `tools/symbolize.sh` on it.

## Gotchas

- The serial device is write-only from the host side (`-serial file:`) — no input channel. Use the GUI window or telnet.
- `telnetd` serves one session; if a client wedges the session, `telnetd STOP` frees it.
- `dmesg` reads the 288-entry ring — long traces overflow; for big captures use `strace -o RAM:file` or read the serial log on the host.
- klog/strace/chiptrace output deliberately bypasses the console — if you "see nothing", check `dmesg` or the serial log, not the shell window.
- `crash` and `format`/`install`-class commands are destructive — do not run them in a session you want to keep.
