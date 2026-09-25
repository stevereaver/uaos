---
type: Kernel Subsystem
title: UAOS Kernel Logging (klog)
description: Unified kernel logging — subsystem/level registry, KLOG emit API, ~47 KB ring buffer, canonical UART driver, and the C:klog / C:dmesg commands.
resource: /kernel/klog/
tags: [kernel, logging, uart, dmesg, strace]
timestamp: 2026-09-25T00:00:00Z
---

# UAOS Kernel Logging (klog)

`kernel/klog/` is the single canonical kernel logging path (UAOS-64). It replaces the ad-hoc `kprint` + per-file UART helper culture that used to exist in `net/ip.c`, `net/dhcp.c`, `net/net_device.c`, `net/ntp.c`, `net/dns.c`, and `drivers/e1000.c` — each of which carried its own copy of COM1 port I/O.

## Architecture

The write path is **ring buffer + UART only**. It never touches VGA, the shell, or the WM paint path, so logging is safe from packet dispatch, IRQ handlers, and M68k emulator callbacks — the contexts that used to lock up when strace printed through `emu_print`/shell output.

```
klog_puts/putc/appendf ──┐
KLOG(...) → klog_emit ───┼→ pending line buffer → ring buffer (always)
klog_raw_feed (kprint) ──┘                           └→ UART (unless raw feed)
```

- `uart.c` — the canonical 16550A COM1 (`0x3F8`) driver: `uart_init`, `uart_putchar` (auto CRLF), `uart_puts`, `uart_write`. `kprint`'s UART output now uses this too.
- `klog.c` — subsystem registry, level thresholds, no-libc printf formatter (`%s %c %d %u %x %p`, zero-pad widths, `l`/`ll` for 64-bit), pending-line assembler, and the ring.

## Levels & Subsystems

Levels `KLOG_OFF < ERR < WARN < INFO < DEBUG < TRACE`; a message emits when `level <= threshold[subsys]`. Every subsystem defaults to `DEBUG`, so serial output is unchanged from the old helper days. 18 subsystems are registered (`kern exec dos vfs net dhcp dns ntp netdev e1000 virtio ide floppy disp audio chip shell strace`); add to the enum in `klog.h` and `k_subsys_names[]` in `klog.c` together.

## Emit API

- `KLOG(subsys, level, fmt, ...)` — preferred one-shot macro; emits `[subsys] formatted text`.
- `klog_puts` / `klog_putc` / `klog_appendf` — streaming API for multi-part lines (the old `_xx_puts`/`_xx_phex` pattern). Fragments accumulate in a pending line committed on `\n` or `klog_commit()`; a mid-line change of subsys/level attribution flushes the pending line first.
- `klog_raw_feed` / `klog_raw_feedn` — unconditional ring-buffer feed with no UART write and no mask check. `kprint`/`kprintbuf` in `uaos_kernel_main.c` use this so boot messages land in `dmesg` while `kprint`'s VGA+UART behaviour is unchanged.

## Ring Buffer

288 entries × ~168 B ≈ 47 KB. Each entry stores a sequence number, subsystem, level, and up to 160 bytes of text (overflow truncates). `klog_ring_get()` iterates oldest-first; `klog_ring_clear()` resets.

## Commands

- **`C:klog`** (`cmd_klog.c`; `debug` is an alias) — `klog` lists subsystems and thresholds; `klog dhcp=off`, `klog net=trace`, `klog all=debug` set runtime masks. `off/err/dbg/none` aliases accepted.
- **`C:dmesg`** (`cmd_dmesg.c`) — dumps the ring oldest-first; `dmesg dhcp` filters by subsystem, `dmesg debug` by minimum level, `dmesg -c` clears.

## strace Integration

`cmd_strace.c` routes trace output through `KLOG_STRACE`: `Strace_ThunkEntry`/`Strace_ThunkExit`/`Strace_DosPacket` feed the original thunk-handler path (`kernel/exec/thunk_handler.c`, `kernel/dos/handler.c`), and `Strace_M68kEntry`/`Strace_M68kExit` hook the **real** dispatch point — `m68k_illg_instr_callback` in `emulation/uaos_m68k_glue.c` — where the kernel dispatches exec/dos/graphics/intuition/gadtools/bsdsocket ILLEGAL-trap libcalls. A `(lib, fn)` name table lets `-e OpenLibrary` filter by function name; `-o <file>` still redirects via VFS. Recursion is prevented by a re-entrancy guard in `trace_output` (klog writes do not themselves thunk).
