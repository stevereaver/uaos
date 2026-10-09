---
type: Kernel Subsystem
title: Exec Library
description: The core system library for task management, memory, and signals.
resource: /kernel/exec/
tags: [exec, tasks, memory, ipc]
timestamp: 2026-06-28T17:47:00Z
---

# Exec Library

The Exec library is the central "kernel" library in UAOS, following the design of Amiga's `exec.library`. It manages the most fundamental aspects of the system.

## Key Responsibilities

- **Task Management**: Creating, scheduling, and switching between tasks (both native x86_64 and emulated M68k).
- **Userspace Execution**: Running Ring-0 native x86-64 ELF64 tasks with memory protection (MMU sandboxing), parent-child hierarchy, and per-task current working directory. (Tasks run in Ring 0 for VirtualBox NEM compatibility; the `INT 0x80` gate keeps `DPL=3` for future Ring-3 re-enablement — see [Userspace & Syscalls](/concepts/userspace_syscalls.md).)
- **System Calls**: Software interrupt `INT 0x80` register-based dispatcher facilitating kernel services for userspace programs.
- **Memory Allocation**: Managing the system memory map and providing allocation services.  `AllocMem` routes `MEMF_CHIP` to the 0–8 MB chip RAM range and `MEMF_FAST` to the 8–16 MB fast RAM range.
- **IPC (Inter-Process Communication)**: Message ports and signals for communication between tasks (including `SIGF_CHILD` for parent notification).
- **Library Loading**: Dynamic loading of native and emulated libraries.
- **MMU Sandboxing**: 4-level paging and memory protection.

## Core Files

- `task.c`: Task creation (native, X64 user-space, and emulated M68k) and context switching logic. Each `UaosTask` carries a `bg_job` field stamped at creation from `g_task_bg_job` — the shell job pump sets that global while dispatching a background job, so `jobs`/`[n] done` bookkeeping can detect when the spawned task(s) actually exit (job numbers never collide with recycled task slots the way a bare `UaosTask *` would).
- `exec_task.c`: AmigaOS-compatible `AddTask`/`FindTask`/`SetTaskPri` helpers for M68k tasks. `SetTaskPri` re-queues a READY task onto the new priority's ready list (under `cli`) so reprioritising applies at the next dispatch instead of silently keeping the old queue position.
- `exec_signal.c`: Task lookup helpers for M68k guest process structures.
- `exec_ipc.c`: Message port and message passing implementation (`NewPort`, `PutMsg`, `GetMsg`, `WaitPort`, `ReplyMsg`).
- `syscall_dispatch.c`: Handling register-based system call routing and implementation for Ring-0 userspace programs.
- `mem_info.c` / `mem_info.h`: Kernel-exported memory query API (`Mem_GetInfo()`), backing both the resident `C:mem` command and the `SYSCALL_MEMINFO` (0x2D) syscall consumed by the on-disk `C:avail` userspace command.
- `elf64_loader.c`: ELF64 PIE/EXEC loader for native x86-64 userspace binaries, plus the x64 heap allocator — see "X64 Heap Allocator" below.
- `loadable_lib.c`: Scans `Workbench:LIBS/` for loadable Amiga `.library` files and registers them with the emulation layer.
- `float_math.c` / `float_math.h`: Shared freestanding IEEE 754 single-precision transcendental helpers (`uaos_sinf`, `uaos_sqrtf`, `uaos_expf`, ...) used by `mathffp.library` and `mathtrans.library`.
- `mmu_sandbox.c`: Paging and memory protection setup for the 4 GB Amiga address space.
- `page_fault_handler.c`: Handles page faults, including custom chip-window accesses from M68k code.  Decodes common `MOV`, `OR`, `AND`, and `XOR` instruction forms.  Installed at IDT vector 14 after `IDT_Init()` so that M68k accesses to the non-present `0x00B00000-0x00DFFFFF` window are emulated rather than raising an unhandled #PF.  Non-chip page faults from X64 userspace tasks kill the task gracefully instead of halting the system.
- `chip_emu.c` (in `kernel/chipset/`): AGA/ECS custom chip emulator with a sparse register dispatch table for the 0xDFF000 register area.  See [Chipset Emulator](/kernel/chipset/index.md).
- `rom_modules.c`: Registers the built-in AmigaOS-compatible libraries at boot.
- `thunk_handler.c`: Native ABI thunk translator for `ILLEGAL` opcode breakout from M68k code.

## AmigaOS-Compatible Libraries and Devices

UAOS provides native thunk implementations of classic AmigaOS libraries and devices for emulated M68k tasks:

- [graphics.library](graphics_library.md) — drawing primitives, fonts, RastPorts, BitMaps, and View/ViewPort management.
- [intuition.library](intuition_library.md) — windows, screens, gadgets, IDCMP, menus, and BOOPSI dispatch.
- [gadtools.library](gadtools_library.md) — high-level gadget creation (buttons, checkboxes, sliders, string/integer gadgets, listviews, cycle gadgets) and layout helpers.
- [dos.library](/kernel/dos/index.md) — file I/O, directories, processes, and AmigaDOS packets.
- [workbench.library](workbench_library.md) — app icons, app windows, and Workbench integration.
- [icon.library](icon_library.md) — guest `DiskObject`s from real `.info` files, tooltypes, and Workbench launch semantics (`WBStartup`, `pr_CLI=0`, project default tools).
- [bsdsocket.library](bsdsocket_library.md) — BSD socket API mapped to the native TCP/IP stack.
- [iffparse.library](iffparse_library.md) — IFF FORM/chunk parser (FORM/LIST/CAT/PROP, SCAN/STEP/RAWSTEP, hooks, properties, collections, local context items).
- [Other Libraries & Devices](other_libraries.md) — `utility.library`, `mathffp.library`, `locale.library`, `ixemul.library`, `console.device`, `keyboard.device`, `timer.device`.

## Preferences Persistence (`prefs_lib.c`)

The preferences library implements AmigaOS IFF PREF file format for `ENV:` and `ENVARC:` preference persistence.

- **IFF PREF format**: `FORM xxxx PREF` containing a `PRHD` (Preference Header) chunk with `do_Type`, followed by type-specific chunks. All chunks use 4-byte type tags + big-endian length + data padded to even.
- **ENV:** — volatile runtime prefs (RAM:ENV), already used by `SetEnv`/`UnSet` shell commands.
- **ENVARC:** — persistent prefs (RAM:ENVARC), survives within a session.
- **API**: `Prefs_Load`/`Prefs_Save` for IFF PREF read/write; `Prefs_FindChunk`/`Prefs_SetChunk`/`Prefs_RemoveChunk` for chunk manipulation; `Prefs_LoadToEnv`/`Prefs_SaveToEnvarc` for ENV:↔ENVARC: copying; `Prefs_NotifyChange`/`Prefs_RegisterNotify` for change broadcast.
- **Prefs types**: `PREFS_WB`, `PREFS_SCREEN`, `PREFS_PALETTE`, `PREFS_POINTER`, `PREFS_INPUT`, `PREFS_FONT`, `PREFS_TIME`, `PREFS_IControl`, `PREFS_SERIAL`, `PREFS_SOUND`, `PREFS_OVERSCAN`, `PREFS_PRINTER`, `PREFS_PGFX`, `PREFS_PPS`, `PREFS_LOCALE`, `PREFS_WBPATTERN`.

## Scheduling Model

Strict priority + round-robin within a priority, like AmigaOS. `task.c` keeps 256 ready lists (`g_ready_heads[pri+128]`) plus a 256-bit occupancy bitmap (`g_ready_map[4]`); dispatch finds the highest non-empty queue via `__builtin_clzll` (O(4) worst case, no 256-entry scan) and dequeues tasks that were marked `TASK_REMOVED` while still queued as a defensive skip. Bitmap clear is followed by a re-check of the queue head so an IRQ-level `ready_enqueue` landing mid-dequeue/`ready_remove` can't leave a set bit for an empty queue (enqueue order is append→set-bit). Spawn priorities: `Idle` is a pure `hlt` loop at `-128` (only runs when nothing else is ready); `EventPump` — the WM/mouse/keyboard/`net_stack_poll`/job-pump loop that used to live inside Idle — is its own signal-driven task at `0` (UAOS-170): it blocks in `Task_WaitTicks(SIGF_EVENTPUMP|SIGF_NET|SIGF_CHILD, 100)` between events instead of spinning a `Forbid`+`hlt` body, so `status FULL` shows it `Wait` when idle. Wake producers call `EventPump_Wake()` (`Signal` +, from IRQ context only — gated on `g_irq_depth` — `Task_ScheduleFromIRQ` for IRQ-exit dispatch): `kbuf_push` (PS/2 and USB-HID keyboard share the ring), the PS/2 mouse packet handler, USB-HID and bcm5974 mouse reports, the RTC 1 Hz tick (menubar clock/blanker), WM `damage_add` (any task's invalidation flushes promptly), and `bg_enqueue` (runback dispatch); NIC RX arrives via the armed `net_rx_notify` slot (`SIGF_NET`), and pump-spawned task exits arrive as `SIGF_CHILD`. The 100-tick timeout is a safety net for unsignalled producers. The pump body runs under `Forbid()` except `ShellWin_PollJobs()` — background-job dispatch is arbitrary command code that legitimately blocks (`CMD_YIELD` sleeps, remote-shell TX waits, DOS packet round-trips), and blocking while `Forbid`'d descheduled the pump mid-critical-section 10×/boot via `run >NIL: C:ntpd` (UAOS-271); `bg_run_next` takes its own `Forbid` for queue surgery only. `Wait`/`Task_SleepTicks`/`Task_WaitTicks` emit `[TASK] WARN ... while critical ... caller=0x..` when entered nested, so new crit-block paths self-report a symbolizable PC instead of just bumping `irqaudit`'s crit-sw counter. It must stay responsive at pri 0 — at -128 strict priority every-tick wakers would starve it (frozen desktop while telnet still works); windowed/remote `Shell`, `telnetd`, and `telnetd-session` run at `0`; shell-spawned M68k and X64 commands inherit `Task_Current()->ln_Pri`. Consequence: a task that never blocks (e.g. `telnetd`'s poll loop, `Idle`) starves all lower priorities if raised above them — `changetaskpri` is now semantically real, verified live (`status FULL` shows mixed priorities; raising `Idle` to 20 froze the rest of the system as expected).

Dispatch happens in the PIT ISR (`do_schedule(1)`), NIC RX IRQ exits (`net_rx_kick` + `Task_ScheduleFromIRQ`), and on demand from task context via `task_switch_away()` (UAOS-169): an `int $0x80`/`SYSCALL_SCHEDULE` trap that lets `do_schedule(0)` arm `Task_SwitchNext` so the syscall-ISR epilogue context-switches immediately on a real saved frame. `int` is a trap, so it works with `IF=0` (the blocking primitives call it inside their `cli` region — the old `sti; int $0x80` shadow concern does not apply). `SYSCALL_SCHEDULE` returns whether a switch was armed (1) or nothing else was runnable (0), so callers fall back to `hlt` instead of spinning on the trap when the ready queues are empty. `Task_Yield()` is a real voluntary reschedule returning that same flag — still a no-op in IRQ context and under Forbid/Disable so it can't break a caller's critical section; `Wait()`/`Task_SleepTicks()`/`Task_WaitTicks()` always deschedule regardless of nesting. Poll loops that must stay off the CPU (shell key/line reads, `shell_yield_ms`, telnetd accept/pump/back-pressure loops, native `sys_read`/`sys_readkey`, `dos_Delay()`) still use `Task_SleepTicks()` — a timed wait on the wait queue woken by `Task_WakeTimers()` each tick — so a runnable-every-tick task doesn't pin the CPU or starve `Idle`.

**Wait-queue invariant**: `Wait()`, `Task_SleepTicks()` and `Task_WaitTicks()` do `wait_enqueue → task_switch_away() → re-check` (UAOS-169): the task parks on the wait queue and hands the CPU to the next ready task immediately, instead of `sti;hlt`-ing as `g_current` until the next PIT tick (the old behaviour idled the CPU for up to 10 ms while other tasks were runnable, which is what made `status FULL` show logically-blocked tasks as `Rdy`/`Run`). When `task_switch_away()` returns 0 — nothing else runnable — an `sti;hlt` fallback keeps the CPU sleeping until the next IRQ. Enqueue is still guarded by `tc_State != TASK_WAITING` — a wake that leaves the task linked must not re-append it (re-appending an already-linked node corrupts the list into a cycle that hangs `Task_WakeTimers()` *inside* the PIT ISR, 100 % CPU, system dead). `Task_WakeTimers()` also unlinks `TASK_REMOVED` stragglers, and the M68k `RemTask` thunk removes a task from the wait queue before marking it removed.

**Deferred reschedule (UAOS-170)**: an IRQ-level `do_schedule(1)` suppressed by `Forbid`/`Disable` nesting sets `g_need_resched` (SysFlags SF_SAR analogue) instead of dropping the wake. `Permit()`/`Enable()` run `Task_CheckResched()` when the nesting count reaches zero: it re-verifies both counters, `g_irq_depth == 0` (an ISR body must never context-switch — `Enable()` is legal inside IRQs, e.g. virtio_net TX) and `IF=1` (raw `cli` regions that bypass `Disable()`), then enters `int $0x80`/`SYSCALL_SCHEDULE` so the switch rides the syscall-ISR epilogue on a real saved frame. `g_irq_depth` is maintained in `ISR_Dispatch` (decremented ahead of the non-returning `Task_Exit()` on a killed X64 task so an abandoned exception frame can't pin it).

**Switch-handoff race (UAOS-180)**: vector `0x80` is a *trap* gate, so IF stays set inside `uaos_syscall_isr`. `do_schedule(0)` updates `g_current` in C while the actual RSP switch happens later in the asm epilogue — an IRQ nesting in that window runs `do_schedule(1)` with `g_current` already naming the incoming task, so the nested `isr_common` epilogue files the physical RSP (still on the outgoing stack) into the wrong task's `native_rsp` and consumes `Task_SwitchPrev/Next` out from under the outer epilogue; the victim later resumes into a garbage frame (`#GP` at the syscall ISR's `iretq`, observed on MBP4,1 once UHCI INTx + sky2 IRQs delivered real traffic). `g_sched_switch_pending` closes the window: `do_schedule` sets it just before `g_current` changes (both directions, so a nested `int $0x80` can't misfile either) and any nested schedule defers via `g_need_resched`; a nested `isr_common` epilogue skips the armed globals entirely; the owning `uaos_syscall_isr` epilogue clears the flag after loading the new RSP.

**One armed switch per interrupt exit (UAOS-181)**: a single ISR can call `do_schedule(1)` several times — the PIT handler runs `timer_ProcessTicks → USB_Poll → HID callback → EventPump_Wake → Task_ScheduleFromIRQ` and then its own `Task_ScheduleFromIRQ`; one UHCI scan can fire the bcm5974 tp and bt callbacks back to back. The second call would see `g_current` = the already-armed incoming task, re-enqueue it and overwrite `Task_SwitchPrev`, so the epilogue filed the outgoing RSP into the wrong `native_rsp` (`#GP(0x98E0)` at the syscall `iretq`, task `bcm5974-reset`, MBP4,1). `do_schedule` now returns early while `Task_SwitchNext` is set — the armed switch stands, anything readied since runs at the next tick. Separately, `isr_common` only performs a switch when `g_irq_depth == 0` (outermost ISR); a nested IRQ (handler re-enabled IF, e.g. `Enable()` from an ISR) leaves the armed switch for the outer epilogue, which runs on the task-level frame.

**CPU accounting + frequency scaling (UAOS-272)**: `do_schedule` now charges `g_current`'s `cpu_ticks` at *entry* (ticks since the last charge) rather than only when a dispatch actually happens — every early-out path (armed switch pending, suppressed reschedule, empty queues, self-dispatch) leaves `g_current` running, so the window always belongs to it. The important consequence: `Idle`'s `cpu_ticks` keeps advancing while it `hlt`-loops alone, which is what makes `taskstat`'s idle share and the cpufreq busy% metric work. `kernel/drivers/cpufreq.{c,h}` adds EIST P-state scaling for the MBP4,1's Core 2 Duo: `CpuFreq_Init` (called from `uaos_kernel_main` before `sti`, gated by `nocpufreq`) probes CPUID.1 ECX bit7 (EST), gates MSR writes to GenuineIntel family-6 models 0x0F/0x17 (Merom/Penryn), and enables EIST via `MSR_IA32_MISC_ENABLE` bit16 — written only when the bit20 lock is clear (a locked MSR can #GP); the MSR readback, not CPUID.ECX.7, is the source of truth since firmware can hide the CPUID bit. C1E/`MSR_POWER_CTL` is *not* touched — 0x1FC is Nehalem+/Atom-only and its rdmsr #GP-panicked the MBP4,1 at boot; Core 2 package C-states are ACPI `_CST`-driven, out of scope. The P-state table comes from ACPI `_PSS` — a minimal AML constant-package scanner walks the DSDT plus every SSDT (`ACPI_FindTableN("SSDT", i)`) handling `Name (_PSS, Package)`, `Method (_PSS){Return(Package|NAME)}`, and `Alias (src, ..._PSS)` indirection; ctl/sts words are written verbatim to `MSR_IA32_PERF_CTL[15:0]` so the FID/VID encoding never needs interpreting. On the MBP4,1 no `_PSS` declaration exists at all — the only CPU SSDT (`Cpu0tSst`) is Apple's `_TSS` *duty-throttling* interface (`Name (TSSI/TSSM)` 5-field packages + a `Method (_TSS)` that recomputes power from a `_PSS` that is never defined; macOS drove EIST via `ACPI_SMC_PlatformPlugin`, not ACPI). A structural fallback (`scan_pkg_any` + `pss_sane`: constant package of ≥2 monotone-MHz 6-int sub-packages with Penryn-plausible FID/VID in ctl) still tries any table, and `cpu DBG` hex-dumps each `_PSS` hit for AML inspection. For model 0x17 the synthesised fallback is a real 8-state Penryn ladder (fid = multiplier, bit6 of the FID byte = +.5 step, 200 MHz bus → 1200–2500 MHz; Core 2 VID = 12.5 mV steps above 712.5 mV, 0x17=1.00 V boot-validated floor up to 0x24=1.16 V at 12.5×, every state's VID clamped ≥ boot VID); other models get a 2-state table (boot PERF_STATUS and the model LFM, FID-sorted descending, same VID — never undervolts); with no EST at all, `MSR_IA32_THERM_CONTROL` bit4 duty modulation (TM1, 12.5%/step) is the coarse fallback. `CpuFreq_Tick` runs from `PIT_IRQHandler` every 10 ticks (100 ms): busy% = 100·(Δpit − Δidle_ticks)/Δpit; ≥25 % busy ramps to the top state, <10 % for ~0.8 s drops to the bottom (hysteresis avoids hunting). QEMU has no EST/_PSS and reports `not GenuineIntel` on TCG — the driver no-ops but the busy% sampler still feeds the menubar. `C:cpu` prints family/model, EIST/C1E/TM1 state, the P-state table, current index, busy%, and PERF_STATUS (`cpu ALL`).

**Queue-surgery atomicity (UAOS-265, the desktop freeze)**: every ready/wait-queue mutation is IRQ-atomic. `ready_enqueue`, `ready_remove`, `ready_dequeue_highest`, `wait_enqueue` and `wait_remove` run inside `irq_save()`, and `do_schedule`'s pick+arm section (prev re-enqueue → dequeue → `g_current`/`Task_SwitchNext` arm) holds one `irq_save` region for the whole sequence. This matters because two caller contexts run with IRQs live: `do_schedule(0)` reached via the `int $0x80` trap gate (IF preserved) from `Task_Yield`/`Task_CheckResched`/`Task_Exit`, and task creation — `Forbid` suppresses dispatch but *not* interrupts, so `Task_CreateNative`/`Task_CreateX64` appended with IF=1. An IRQ-side `Signal()`/`do_schedule(1)` landing mid-append tears the list: the node stores land but the head stores don't (or vice versa), producing the observed split-brain — EventPump READY-but-self-linked on metal, half-linked into a self-linked pri-0 head under QEMU — a task the scheduler can never reach again (dead input/WM/cursor while telnetd stayed up). `Task_CreateX64` also unlinks a stale-linked REMOVED node before memset-reusing its slot, and new slots stay `TASK_REMOVED` until `ready_enqueue` births them so IRQ-side walkers can't see a half-built task. As a safety net `Task_WakeTimers` scans `g_tasks` each tick for a READY/WAITING task with a self-linked node (≠ `g_current`) and re-links it after two consecutive sightings — a strand now self-repairs and logs `[TASK] WARN: '<name>' stranded` instead of freezing the desktop silently.

**Task-exit cleanup hooks**: `Task_Exit()` is the single funnel for native/X64 task teardown (the M68k `RemTask` thunk bypasses it, but only operates on `TASK_TYPE_M68K` tasks via `Task_FindByM68kAddr`, so native tasks always reach it). It runs a hook chain for resources a task may abandon mid-lifecycle: `UAOS_Intuition_CleanupTask` (guest windows/screens, UAOS-265 — also hooked directly from `stub_RemTask` since the victim's RAM window must still be bound), `usock_cleanup_task` (bsdsocket fds), `Telnetd_CleanupTask` and `ShellWin_RemoteCleanupTask` (telnetd pump contexts, remote-shell slots and their sockets, UAOS-263 — a pump or remote `Shell` task dying without its own exit path would otherwise pin an `ESTABLISHED` socket/`remote_inuse` slot forever), `net_rx_notify_disarm` (per-task RX-notify slot), and — for `TASK_TYPE_M68K` victims — `UAOS_M68k_ReleaseTaskResources` (audio.device channels + open counts, plus pending device-request purges — timer queue, console/keyboard pending reads — UAOS-240) plus `HandleTable_FreeByOwner`/`VFS_FreeLock` (DOS files and locks, with `ACTION_FREE_LOCK` sent to the owning handler so its lock nodes die too, UAOS-247). `Task_ReleaseM68kRam` closes the chain: `chip_emu_unbind_ram`/`floppy_unbind_ram` disarm every DMA engine bound to the window (display/copper, blitter, audio, disk) and `UAOS_Heap_ReleaseWindow` drops the `g_heap_heads[]` free-list mirror for that window — both are keyed by the window pointer, so without them the next tenant of a recycled slot either gets scribbled on by stale DMA or inherits the dead task's freelist head pointing into its fresh program image (the UAOS-234-style decrunch corruption).

## Task Stack Alignment

The x86-64 SysV ABI requires the stack pointer to be 16-byte aligned *before* a `CALL` instruction, so a function is entered with `%rsp ≡ 8 (mod 16)` (the return address pushed by `CALL`). GCC relies on this for aligned SSE spills (`movaps [rsp]`); a task whose entry RSP is wrong runs misaligned for its whole life and #GPs on the first such spill.

Key details:
- `g_task_stacks` slots are 32 KB each, so every stack top is 16-byte aligned.
- `Task_CreateNative()` plants a return-address slot holding `Task_Exit` just below the stack top and puts that address (top − 8, ≡ 8 mod 16) in the synthetic frame's RSP slot, so `iretq` enters the task exactly as if it had been `CALL`ed, and returning from the entry function exits the task cleanly (UAOS-182). Before this fix the frame used RSP = top: every native task except the first was misaligned, and the desktop Shell #GP'd in `draw_menubar` when a command redrew the desktop from Shell context.
- `Task_RunNew()` (first task only) gets there differently: `and rsp,-16; call entry`.
- `Task_CreateX64()` uses the ELF loader's `initial_rsp` (process-entry convention: RSP 16-aligned at `_start`, no return address).
- Synthetic frames are 22 qwords (R15..RAX, vector, error_code, RIP, CS, RFLAGS, RSP, SS); X64 tasks use kernel CS/SS `0x08`/`0x10` and `iretq` still pops SS in 64-bit mode.
- `isr_common` and `uaos_syscall_isr` use the same frame layout for both the interrupted task and the task being switched to. The CPU 16-aligns RSP before pushing an interrupt frame and `iretq` restores the original RSP, so interrupts never change a task's alignment parity.

## X64 Syscall Dispatch

X64 userspace tasks communicate with the kernel via INT 0x80 syscalls (`syscall_dispatch.c`). Key syscalls include `read`, `write`, `open`, `close`, `exit`, `getargs`, `spawn`, `wait`, `alloc`, `getcwd`, `opendir`, `readdir`, `stat`, GUI window operations (0x11–0x18), extended GUI drawing primitives (0x30–0x37), the filesystem metadata syscalls (`SYSCALL_MKDIR` through `SYSCALL_GETMOUNTNAME`, 0x20–0x2C), `SYSCALL_SEEK` (0x2F — file-position seek, added for `wget -c` resume; UAOS-143), and the userspace socket block (`SYSCALL_NET_SOCKET` through `SYSCALL_NET_STATE`, 0x38–0x3F — TCP connect/send/recv/close, DNS resolve, timeouts, state; see [TCP/IP Network Stack](/kernel/net/index.md)). `SYSCALL_TIME` (0x40) returns the UTC epoch (`ntp_get_epoch()`, 0 until ntpd syncs) and `SYSCALL_GETRANDOM` (0x41) fills a buffer from `kernel/drivers/entropy.c` — added for the BearSSL TLS layer (UAOS-147). `SYSCALL_FREE` (0x42) releases an x64 heap block allocated by `SYSCALL_ALLOC` — added for per-block userspace deallocation (UAOS-152).

### Timed Sleep (`SYSCALL_SLEEP_MS`, 0x2E) and Task Wake Timers

`sys_sleep_ms` blocks the calling task for a wall-clock duration (UAOS-120). `Task_SleepTicks(ticks)` in `task.c` parks the task on the wait queue — same immediate-switch mechanism as `Wait()` (UAOS-169), but with `tc_SigWait = 0` so signals do not wake it — and stamps `tc_wake_tick` (an absolute `g_pit_ticks` deadline, 100 Hz = 10 ms/tick). `Task_WakeTimers()` is called from `PIT_IRQHandler` before `Task_ScheduleFromIRQ()` and re-readies wait-queue tasks whose deadline has passed. This replaced yield-counting delays (e.g. `gnu:sleep` looped `uaos_yield()` ~100×/sec assuming 10 ms per yield, which actually cost a full round-robin quantum — `sleep 3` took ~15 s and burned CPU); now `sleep 3` completes in ~3 s while the task truly blocks.

### Memory Query API (`SYSCALL_MEMINFO`, 0x2D)

`sys_meminfo` fills a `struct uaos_meminfo` (kernel side: `struct UaosMemInfo` in `mem_info.h`) with a point-in-time snapshot of the live memory arenas. It is a thin wrapper over the in-kernel `Mem_GetInfo()` helper in `mem_info.c`, which gathers:

- **x86-64 userspace heap** — total/used/free from the ELF64 loader arena (`ELF64_HeapSize()` / `ELF64_HeapUsed()`). This arena backs ELF64 segment loading, initial stacks, and `sys_alloc`/`sys_free`; `used` counts live allocated block bytes, so it shrinks on `free` and on task exit (per-task blocks are reclaimed by `ELF64_FreeTaskBlocks()`, and `ELF64_ReclaimHeap()` resets the whole arena once no X64 tasks remain).
- **Emulated M68k guest RAM slots** — per-task RAM pool count from `Task_M68kSlotCount()` and the per-slot size (`GUEST_RAM_SIZE`).
- **Scheduler task table** — total/running/waiting counts from `Task_GetCounts()`. `running` counts both `TASK_RUNNING` and `TASK_READY` (same runnable-set semantics as `Task_RunnableCount()`), matching the `tasks_running` field's documented "RUN/READY state" contract.

The same `Mem_GetInfo()` helper is consumed directly by the resident `C:mem` command, so kernel and userspace memory reports stay consistent. The on-disk `C:avail` userspace command queries this API to render real memory statistics.

### Userspace GUI Widget Toolkit (`uaos_gui.h`)

Native x86-64 userspace tasks have access to a Workbench-style widget toolkit via `system/libuaos/uaos_gui.h`. This header-only library provides AmigaOS 3.1 GadTools-compatible gadget classes built on the syscall drawing primitives.

**Widget types**: Button, Checkbox, Radio button, Slider, String gadget, Integer gadget, Label, Listview.

**Extended drawing syscalls** (0x30–0x37) back the toolkit:
- `SYSCALL_GUI_DRAW_LINE` (0x30) — Bresenham line drawing.
- `SYSCALL_GUI_FILL_RECT` (0x31) — Filled rectangle.
- `SYSCALL_GUI_DRAW_3DBORDER` (0x32) — Raised/recessed 3D bevel with auto-shading.
- `SYSCALL_GUI_DRAW_PIXEL` (0x33) — Single pixel.
- `SYSCALL_GUI_DRAW_TEXT_BG` (0x34) — Text with foreground and background colors.
- `SYSCALL_GUI_GET_WINSIZE` (0x35) — Query window client area dimensions.
- `SYSCALL_GUI_SET_TITLE` (0x36) — Update window title bar text.
- `SYSCALL_GUI_DRAW_ELLIPSE` (0x37) — Ellipse outline (midpoint algorithm).

**Kernel-side implementation**: `kernel/display/user_window.c` implements all drawing primitives using a per-window backing buffer. The 3D border helper auto-computes highlight/shadow colors from a base color. The ellipse renderer uses the midpoint ellipse algorithm with 4-way symmetry.

**Userspace API**: `uaos_gui_init()` binds a GUI context to a window handle. `uaos_gui_create_gadget()` allocates widgets from a `uaos_newgadget_t` descriptor. `uaos_gui_handle_event()` dispatches mouse/keyboard events to the appropriate widget (button press, checkbox toggle, radio group selection, slider drag, string cursor/edit). `uaos_gui_poll()` is a combined event-loop helper that polls, handles, and redraws. `uaos_gui_get_int()`/`uaos_gui_set_int()` and `uaos_gui_get_str()`/`uaos_gui_set_str()` query and update widget state. `uaos_gui_draw_group()` renders a recessed frame with title text for visual grouping.

### stdin read (`sys_read`, fd=0) and `sys_readkey`

Both syscalls take their input from the task's **key source** — `UaosTask::key_src`, an opaque pointer to the owning `ShellInstance` set when the shell spawns the task and inherited by `sys_spawn` children (UAOS-287). With a key source, `sys_read`/`sys_readkey` drain that shell's keyboard ring via `ShellWin_KeySrcGet()`, so remote (telnet) sessions — where `ShellWin_RemoteFeed()` enqueues NVT-decoded bytes — can satisfy interactive reads (`more`, `dir KEYS/INTER`, stdin filters). `ShellWin_KeySrcGet` returns -1 when the owning remote session is dead, letting blocked commands exit instead of hanging forever; `C:more` treats `<0` and `0x03` (Ctrl-C) as quit. With no key source (key_src NULL) the syscalls fall back to the PS/2 keyboard as before. When no key is available the wait calls `Task_SleepTicks(1)`, which parks the task on the wait queue and context-switches away immediately (UAOS-169); `Task_WakeTimers()` re-readies it at the next tick and it re-checks for input when re-dispatched.

### Trap Gate for Vector 0x80

The INT 0x80 syscall gate is configured as a **trap gate** (IDT type 0xEF), not an interrupt gate (0xEE). A trap gate does not clear IF on entry, so interrupts remain enabled during syscall handlers. This is essential: `sys_read` and `sys_readkey` loop waiting for keyboard input and rely on the timer ISR to preempt them. With an interrupt gate, the timer ISR could not fire during a blocking syscall, freezing the entire UI.

### CPU Exception Handling (ISR_Dispatch)

`ISR_Dispatch` in `irq/idt.c` handles all IDT vectors. For CPU exceptions (vectors 0-31) with no registered handler, it checks whether the faulting task is an X64 userspace task. If so, the task is killed via `Task_Exit()` (printing a diagnostic message first) and the scheduler picks the next runnable task. This prevents a single buggy userspace command (e.g. a GNU coreutils binary that triggers a GPF) from locking up the entire OS. Kernel-mode exceptions still halt the system as a fatal panic.

## X64 Heap Allocator

The 4 MiB static arena in `elf64_loader.c` (`g_x64_heap`) backs every x86-64 userspace allocation: ELF image reservation, the initial stack, and `SYSCALL_ALLOC`. It is a boundary-tag free-list allocator (UAOS-152), replacing the original bump pointer.

- **Layout**: a doubly-linked chain of blocks ordered by address. Each block's 48-byte header (`X64Blk`: `magic`, `size`, physical `prev`/`next`, `owner`, `flags`) sits immediately before its payload, so `free()` finds its header at `ptr - 48` and prev/next describe physical adjacency — neighbour coalescing is O(1).
- **Allocation** is first-fit with splitting: a lead fragment is split for alignment (16-byte minimum; 4096 for PIE image bases), a tail fragment when the remainder can still hold a header plus 16 bytes. A lead sliver under 48 B is absorbed into the preceding used block (or orphaned at the arena base for the first block) rather than creating an invalid header-sized fragment.
- **ET_EXEC reservation**: `x64_heap_reserve()` marks an absolute address range used — required for fixed-vaddr images — splitting around the range. It fails if the range overlaps live memory or the containing free block starts within 48 B of the image (the header would be clobbered by the segment copy).
- **Ownership**: each used block records its owning `UaosTask *`. `sys_alloc` stamps `Task_Current()`; loader blocks start ownerless and are handed to the new task by `ELF64_HeapOwn()` after `Task_CreateX64` succeeds. `Task_Exit` frees all blocks a dying task still owns via `ELF64_FreeTaskBlocks()` — only header fields are written, so freeing the stack the exit path itself runs on is safe.
- **Validation**: free rejects NULL (no-op), out-of-arena, bad magic, double-free (`!USED`), insane size, and inconsistent prev/next links — each rejection logs to klog (`[ELF64] free: ...`) and leaves the heap untouched.
- **Reclamation**: `ELF64_ReclaimHeap()` still runs from `Task_Exit` and resets the arena to a single free block once no X64 task remains — now acting as a defrag/sweep for ownerless strays rather than the sole reclaim path.
- **Accounting**: `ELF64_HeapUsed()` returns bytes held in live used blocks (`g_x64_live`), not a high-water mark — `avail`/`SYSCALL_MEMINFO` reflect real occupancy.
- **Concurrency**: all list mutation runs under `irq_save()`/`irq_restore()` critical sections (the pushfq/cli + conditional-sti idiom, same as `Memcheck_Scan`), so a PIT preemption can never observe a half-split chain.

## M68k Integration

Exec provides the bridge for emulated M68k tasks, including "LVO" (Library Vector Offset) stubs that allow M68k code to call native C functions.

Library bases live in `emulation/uaos_m68k_glue.c`: `EXEC_BASE = 0x1000` (negative stub band `0xC1C–0xFFA`, positive ExecBase image above), `DOS_BASE = 0x2000` (stub band `0x1C1C–0x1FFA`), then bsdsocket `0x3000`, graphics `0x8000`, intuition `0x9000`, gadtools `0xA000`, audio.device `0xE000`, generic fake `0xF000`. Both exec and DOS pre-fill every LVO slot `-6`…`-996` with a catch-all stub before overlaying implemented functions — nothing in the exception-vector page. `exec.library/StackSwap` (`-732`, `EXEC_STACK_SWAP`) swaps the task's `tc_SPLower`/`tc_SPUpper`/SP with a guest `StackSwapStruct`, migrating the jsr'd return address onto the new stack AROS-style so the stub's RTS returns the caller on the new stack. `libmap_selfcheck()` asserts the bands are pairwise-disjoint on every table install. The same constants are duplicated in `kernel/exec/exec_task.c` and `kernel/exec/dos_lib.c` — keep all three in sync (UAOS-252).

### exec.library coverage expansion (UAOS-239)

The guest-visible `exec.library` table was filled out to the OS-2.x application set. Implemented: `Supervisor`/`SetSR`/`GetCC`; `AllocAbs`/`Allocate`/`Deallocate`/`AddMemList` (exact-address allocation via `dos_AllocAbs_glue` — the heap header lands 8 B before the payload, so `AllocAbs` must reserve the preceding `HEAP_HDR` bytes and split busy ranges around them); `AddTask`/`RemTask`/`FindTask` (walks `TaskReady` then `TaskWait`); `AddLibrary`/`RemLibrary`, `AddDevice`/`RemDevice`, `AddResource`/`RemResource` (priority-enqueue/remove on the matching ExecBase lists); `SumLibrary`/`SumKickData`/`Debug`/`Alert` (logged stubs); `RawIOInit`/`RawMayGetChar`/`RawPutChar` (serial I/O — `RawPutChar` is how headless tests observe guest output); `FindResident`/`InitResident` (RomTag scan/init); `MakeFunctions`/`MakeLibrary` (builds jump tables + Library structs in caller-supplied or allocated guest RAM); `CreatePool`/`AllocPooled`/`FreePooled`/`DeletePool`; `CachePreDMA`/`CachePostDMA`; `AddMemHandler`/`RemMemHandler`; `Child*` stubs; semaphore lists (`InitSemaphore`, `ObtainSemaphoreList`, `ReleaseSemaphoreList`, `FindSemaphore`, `AddSemaphore`, `RemSemaphore`, `AttemptSemaphoreShared`) — the `SignalSemaphore` layout uses `ss_NestCount@14`/`ss_Owner@28`, 46-byte struct. `FindName`/`FindTask` share `glue_find_name`, which stops the walk on a corrupted chain instead of running into data.

Regression coverage: `system/Demos/src/ExecTest.s` is a guest M68k program that calls each new vector directly and reports `EXECTEST PASS`/`FAIL <stage>` over `RawPutChar`; `tests/qemu_exec_coverage_test.py` reassembles the per-character stream from the `[trace]` log (`lib=1 fn=98` = `RawPutChar`). Two guest-side pitfalls found while writing it: `MOVEA` does not set condition codes (test pointers need an explicit `tst.l` before `bne`), and calling a `MakeLibrary`-generated vector table in fast RAM legitimately enters `0x800000+` then returns into the `0x20000–0x80000` band — which trips the (fatal) LOW-REENTRY decrunch heuristic — so the test inspects the generated JMP bytes instead of calling through them.

## Guest Memory Layout

The emulated M68k guest RAM is wired into the 4 GB guest physical window at offset `0x00000000`.  `GUEST_RAM_SIZE` is defined as 16 MB, split into:

- **Chip RAM**: `0x00000000–0x007F0000` (8 MB minus a 64 KB guard)
- **Fast RAM**: `0x00800000–0x00FF0000` (8 MB minus a 64 KB guard)

`exec.library` `AllocMem` honours the request flags:

- `MEMF_CHIP` / `MEMF_DMA` / `MEMF_24BITDMA` allocate from chip RAM.
- `MEMF_FAST` allocates from fast RAM.
- Unspecified / `MEMF_PUBLIC` allocations prefer fast RAM and fall back to chip RAM.

The Amiga custom chip / CIA register window at `0x00B00000–0x00DFFFFF` is mapped non-present; accesses from M68k code fault to the page fault handler and are forwarded to the chip emulator in `kernel/chipset/chip_emu.c`.

## Allocation tracking on task exit (memcheck)

When `memcheck` is on, every tracked `AllocMem` records its allocating task (`MemchkRec.owner` = `Task_Current()` at alloc time) alongside the task-name label. `Task_Exit` calls `Memcheck_FreeByOwner()` for M68k tasks while `g_ram` still maps the dying task's guest RAM, so blocks a guest allocated are reclaimed on both normal `Exit()` and an external `m68k_halted` abort (UAOS-247 — the old cumulative cycle-budget kill was replaced by an unblocked-spin watchdog that is diagnostic-only) — `memcheck` shows 0 live tracked allocs afterwards instead of leaking records that could alias a recycled RAM slot's new allocations.

### Per-window records (UAOS-266)

`g_ram` is rebound on every context switch — each M68k task runs against its own 16 MB window and native tasks see `g_shared_ram` — so a guest address alone does not identify a block. Every `MemchkRec` therefore also stores `ram` = the `g_ram` window the allocation came from:

- Guard checks (`mc_check_guards`) read through the record's own window, not the caller's binding — `Memcheck_Scan` run from the shell/telnet (shared window) no longer reports every live M68k-owned block as FRONT+TAIL corrupt.
- `mc_find` prefers a same-window match (guest addresses are only unique per window) and falls back to a cross-window match so native teardown paths can free a guest's block into the heap it came from; `mc_free`/`Memcheck_FreeByOwner` rebind `g_ram` IRQ-off around the guard check + freelist free.
- `Memcheck_Scan` walks the free lists of *every* registered window (`g_heap_heads[]`, the host-side pool-head table already keyed by window) rather than only the caller's — log lines label pools `chip[n]`/`fast[n]` by window index.
