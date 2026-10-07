---
type: Kernel Subsystem
title: UAOS Diagnostics Toolkit
description: Kernel diagnostic modules (watchdog, tickmon, etrace, prof, failalloc), the C: debug command suite, serial console, in-guest pcap, and host-side decoders.
resource: /kernel/dbg/
tags: [kernel, debug, diagnostics, watchdog, etrace, prof, sercon, pktmon]
timestamp: 2026-10-01T00:00:00Z
---

# UAOS Diagnostics Toolkit

Roadmap card: UAOS-188 (children UAOS-195..216).  Goal: make silent hangs,
scheduler corruption, IRQ-routing failures, and allocator failures
inspectable without GDB — including on hardware like the MacBookPro4,1
where serial output may not exist and a wedge looks like a frozen desktop.

## Shared emit plumbing (`kernel/dbg/diag.h`)

All dump APIs take `(void *ctx, DiagEmitFn emit)` — the producer builds a
line with the `DiagLine` helpers (`dl_add/dl_dec/dl_hex/dl_pad/dl_emit`)
and hands it to the sink.  The same dump drives `C:` commands
(`ctx->print`), the serial console (`uart_puts`), and the watchdog's
klog fallback.  `diag_rdtsc()` is the shared TSC read used by the
sampling modules.

## Kernel modules (`kernel/dbg/`)

| Module | Purpose | Entry points |
|---|---|---|
| `watchdog.c` | Stall detector (UAOS-198). PIT-tick driven: trips if `g_ctx_switches` doesn't advance within the budget while ≥2 tasks are runnable, or if the tick ISR itself stops (armed via RTC second tick). | `Watchdog_SetBudget/Tick/RtcSecond/Test`, `watchdog=<ms>` cmdline, `C:watchdog` |
| `tickmon.c` | Tick/jitter instrumentation (UAOS-208): PIT period min/avg/max vs self-calibrated TSC Hz; per-vector IRQ dispatch latency (log2 histogram + worst-cycle table). First delta after seeding is dropped — the seed can be a PIT edge latched while IF=0 during init, delivered at arbitrary phase (UAOS-270). Extremes are attributed via `pit_min_idx`/`pit_max_idx`/`pit_min_prev` so short deltas classify as delayed-tick complements vs lone early dispatches. | `Tickmon_PitTick/IrqDur/Snapshot/TscHz`, `C:tickcheck` |
| `etrace.c` | Binary event trace ring — ftrace-lite (UAOS-209). 16384 × 16 B records `(tsc, event, arg0, arg1)`; class mask keeps disabled emission at ~2 cycles. Avoids the UART starvation that killed `klog all=trace` sessions. | `Etrace_Emit/SetMask/DumpFile/Tail`, `C:etrace` |
| `prof.c` | PIT-sampled RIP profiler (UAOS-210). Samples the outermost stashed `IsrFrame` RIP + task index each tick into an open-addressed table. | `Prof_Start/Stop/Tick/Report/DumpFile`, `C:prof` |
| `failalloc.c` | Deterministic allocation-failure injection (UAOS-211): 1-in-N or every-after-N with LCG seed. Both heaps: guest `AllocMem` (`exec/dos_lib.c`) and x64 userspace heap (`exec/elf64_loader.c`). | `Failalloc_Config/ShouldFail/Status`, `C:failalloc` |
| `sysinfo.c` | Live hardware inventory for `showconfig`/`version` (UAOS-278): MB2 memory-map total, CPUID brand string + cpufreq MHz, PCI display-class device, `IRQ_Mode()`/`ACPI_LapicBase()`, `UAOS_PIT_HZ`, 16550 scratch-reg probe, `PS2_Present()`, USB-HID/UHCI counts, netdev name, BlockDev list. | `SysInfo_Init/RamBytes/CpuBrand/InputDesc/DumpConfig`, `showconfig` builtin |

### Trace/prof instrumentation points

- `ISR_Dispatch` (`kernel/irq/idt.c`): `ETRACE_IRQ_ENTER/EXIT`, per-vector
  dispatch-cycle measurement → `Tickmon_IrqDur`, outermost-frame stash for
  `IDT_LastIsrFrame()` (prof/watchdog sampler).
- Scheduler switch (`kernel/exec/task.c`): `ETRACE_SCHED`, per-task
  `cpu_ticks`/`ctx_switches` charging, Disable-hold accounting
  (`irqoff_ticks`, `irqoff_max_ticks`, `irqoff_long`,
  `switch_while_crit`), stack-canary check on switch-out.
- `Signal()` → `ETRACE_SIGNAL`; `DoPkt`/`SendPktAsync` → `ETRACE_DOPKT`;
  `netdev_send`/`stack_rx` → `ETRACE_PKT_TX/RX` + `Pktmon_Record`.
- PIT handler (`uaos_kernel_main.c`): `Tickmon_PitTick`, `Prof_Tick`,
  `Watchdog_Tick`.  RTC IRQ8 → `Watchdog_RtcSecond`.

## Scheduler/task instrumentation (`kernel/exec/task.{c,h}`)

`UaosTask` carries diagnostic fields: `cpu_ticks`, `ctx_switches`,
`irqoff_*`, `switch_while_crit`, `disable_enter_tick`,
`stack_overflowed`.  Every native stack is filled with `0xA5` at creation
with a `STACK_CANARY` qword at the base; `Task_StackPeakUsed()` scans the
fill for the high-water mark and the canary check runs at every context
switch (dead canary → klog + flag, reported by `taskdump`/`stack`).
`Task_DiagDump()` walks `g_tasks[]` and decodes the parked interrupt
frame at `native_rsp` (same layout as `isr_common`) — RIP/CS/RFLAGS/RSP/SS
plus r15..rax.

Reading the watermark (UAOS-228): interrupt handlers run on the
*interrupted* task's stack — there is no dedicated IRQ stack — so a
task's peak includes whatever ISR work landed while it was current.
`Idle`'s high peak is genuine IRQ-context usage, not a boot artifact:
`uaos_kernel_main` runs on a dedicated 64 KB BSS stack
(`stack_bottom`/`stack_top` in `uaos_kernel_entry.asm`) before tasks
start, while the PIT handler calls `timer_ProcessTicks()` →
`chip_emu_beam_tick()`/`chip_emu_run_to_cycle()` (per-scanline DMA state
machines), `USB_Poll`, `net_stack_tick`, `Task_WakeTimers`,
`Watchdog_Tick`, and `Task_ScheduleFromIRQ`. Any Idle watermark is the
deepest of those chains, so Idle (or any usually-current task) can
report a peak far above what its own entry function uses.

## New C: commands

| Command | What it does |
|---|---|
| `taskdump [TASK=n|FULL]` | Live task table: state, pri, cpu ticks, ctx switches, stack peak, wait mask, nest levels; per-task saved-frame decode. (UAOS-195) |
| `taskstat [<sec>\|NOW]` | Per-task CPU% + switches/s sampled over a window; `NOW` prints cumulative counters under `cpu_t`/`switches` headers (no window). (UAOS-197, NOW-mode headers UAOS-282) |
| `watchdog [MS=n\|OFF\|TEST]` | Arm/status/self-test the stall watchdog. `TEST` holds `Forbid()` past the budget — interrupts still fire, no reschedules — exactly the condition it guards. (UAOS-198) |
| `ports` | Handler MsgPort pending queues + async packet pool. (UAOS-200) |
| `timers` | Pending `TimeRequest`s: fire tick, delta, sigmask, owner. (UAOS-201) |
| `handles` | Open-file/lock handle table with owning task + flags. (UAOS-202) |
| `netstat` | TCP sockets (state/addrs/queue depths/retx), UDP sockets, usock layer. (UAOS-203) |
| `diskdiag [TEST=dev]` | Blockdev registry + IDE/AHCI/VirtIO-blk/VirtIO-scsi/floppy stage dumps with live status regs; `TEST=` does a timed 1-sector read. (UAOS-204) |
| `pciscan [BUS=n]` | Full PCI enumeration: bdf, class, vendor:dev, pin/line, BARs (64-bit BARs combined). Header-type aware: type-1 bridges get `sec=`/`sub=` bus numbers plus decoded `io`/`mem`/`pmem` forwarding windows on a continuation line; CardBus shows 1 BAR. (UAOS-199, bridge decode UAOS-282) |
| `irqroute [BDF=b:d.f]` | pin→PIRQ→GSI→vector decode per device: ICH DxxIP/DxxIR override, PIRQA-H→IO-APIC 16-23, intline fallback, resolved vector + name. `vec=UNASSIGNED` and `* UNRESOLVED ROUTE *` are the verdicts. (UAOS-199) |
| `peek <addr> [LEN=n] [W=8\|16\|32\|64]` | Physical/MMIO read (identity map). (UAOS-205) |
| `poke <addr> <val> [W=..] FORCE` | MMIO write; refuses without `FORCE`, reports before/write/readback. (UAOS-205) |
| `irqaudit` | Per-task Disable/Forbid audit: current nest, total IF=0 time, max hold, >50 ms holds (threshold is `TASK_IRQOFF_LONG_MS` converted via self-calibrated `Tickmon_TscHz()`, with a ~50M-cycle fallback before calibration), descheduled-in-crit count. (UAOS-206, threshold fix UAOS-282) |
| `sercon ON\|OFF` | Serial console control (below). (UAOS-207) |
| `tickcheck [SEC=n]` | PIT period vs calibrated TSC, IRQ latency histogram, worst vectors; `SEC=` measures TSC/PIT drift in ppm. (UAOS-208) |
| `etrace [MASK=n\|OFF\|TAIL n\|FILE=path]` | Event ring control; `FILE=` writes the binary dump for `tools/etrace_decode.py`. (UAOS-209) |
| `prof START\|STOP\|REPORT [n]\|FILE=path` | RIP profiler; `FILE=` writes `rip taskidx count` for `tools/prof_report.py`. (UAOS-210) |
| `failalloc ON RATE=n\|AFTER=n [SEED=n] \| OFF` | Alloc-failure injection. (UAOS-211) |
| `stack` | Now prints per-task peak usage from the fill watermark + canary state. (UAOS-212) |
| `pktmon START FILE=path [MAX=n] \| STOP` | In-guest pcap writer at the netdev TX/RX tap — works on bare metal where QEMU `filter-dump` can't. (UAOS-214) |

## Boot cmdline additions (`Mb2_CmdlineParam`)

- `watchdog=<ms>` — stall budget at boot (default armed at 5000 ms;
  `watchdog=0` disables).
- `sercon` / `console=ttyS0` — start the serial console task at boot.

## Serial console (`kernel/klog/sercon.c`, UAOS-207)

uart.c now has a polled RX half (`uart_rx_ready`/`uart_getchar`); sercon
is a low-priority native task that polls LSR and executes a minimal
command set (`help ps taskdump dmesg irqstat klog mem tick reboot`) over
COM1.  Polling is deliberate — it works when interrupt routing itself is
what's broken.  Under `-serial file:` the guest sees no RX data, so for
an interactive serial session use `-serial tcp:...` or a pty; output
still lands in the log either way.

## Host-side tools

| Tool | Purpose |
|---|---|
| `tools/gdb_uaos.py` | GDB helpers (`uaos tasks`, `uaos task NAME`, `uaos timers`, `uaos stack NAME`) — walks `g_tasks[]` via DWARF, decodes parked frames. (UAOS-213) |
| `tools/etrace_decode.py` | Decodes the `ETRC` binary dump (12 B header + 24 B records). (UAOS-209) |
| `tools/prof_report.py` | Symbolizes `prof FILE=` output into a sorted hotspot table via `nm`. (UAOS-210) |
| `tools/analyze_log.py` | Serial-log analyzer: panic extraction + symbolization, warn/err rollup, watchdog events, boot markers. (UAOS-216) |
| `tests/smoke.sh` | Headless QEMU + telnet command battery; archives serial log + pcap under `build/smoke-<ts>/`. (UAOS-215) |

## IRQ/PCI diagnostic exports (`kernel/irq/irq.{c,h}`)

`IRQ_PciRead8/16/32`, `IRQ_VecKind/VecGsi/VecForGsi`, and
`IRQ_RouteInspect()` — a read-only pin→GSI decode that mirrors the
resolver's ICH DxxIP/DxxIR + intline fallback logic without mutating
routing state.  `IRQ_DiagIch()` snapshots the ICH PIRQ/DxxIP/DxxIR/OIC
register set (shared with `usbdiag`).
