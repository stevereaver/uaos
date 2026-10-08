---
type: Emulation Layer
title: M68k Emulation Layer
description: Integration of the Musashi M68k emulator and the UAOS kernel.
resource: /emulation/
tags: [m68k, musashi, glue]
timestamp: 2026-08-30T15:00:00Z
---

# M68k Emulation Layer

The emulation layer provides the infrastructure to execute Motorola 68000 binary code on the x86_64 UAOS kernel.

## Core Components

- **`uaos_m68k_glue.c`**: The primary interface between the Musashi emulator and the kernel. It handles CPU initialization, memory access callbacks, opcode trapping, and the Amiga Hunk binary loader.
- **`uaos_emu_registry.c`**: Manages embedded Amiga binaries (e.g., `Lha`) and exposes `UAOS_Emu_RunByName()` for the shell.
- **`uaos_uae_bridge.c`**: UAE-compatible bridge that wires ILLEGAL callbacks and reserves the 4 GB guest RAM window — a demand-paged VA reservation at 16–20 GB committed per 2 MB page on first touch via `UAOS_VM_ReserveGuestWindow`/`UAOS_VM_GuestWindowFault`/`UAOS_VM_ReleaseGuestWindow` in `kernel/exec/mmu_sandbox.c` (UAOS-162). `UAOS_Bridge_PostInitProbe()` verifies the commit path after the #PF handler is installed.
- **`uaos_m68kconf.h`**: Musashi configuration tuned for M68020 emulation (the 68020 core is a superset of the 68000, so 68000 code runs unmodified) with ILLEGAL/TRAP callbacks and no FPU/PMMU. The CPU type is selected at runtime in `uaos_m68k_glue.c` via `m68k_set_cpu_type(M68K_CPU_TYPE_68020)`.
- **`rom_patches/`**: Contains assembly stubs (`rom_traps.s`) and the kickstart configuration (`kickstart.conf`).
- **`binaries/`**: Embedded Amiga binaries converted to C byte arrays (e.g., `Lha`).

## Execution Model

Emulated tasks are created as `UaosTask` objects with an M68k context. When the scheduler switches to an M68k task, it invokes the Musashi `m68k_execute()` function. The scheduler supports multiple M68k tasks, each with its own 16 MB guest RAM pool (8 MB chip + 8 MB fast).

## Guest Memory Layout (Musashi Build)

Each M68k task receives a 16 MB guest RAM window mapped into the host address space. The first 8 MB are chip RAM and the second 8 MB are fast RAM:

| Region | Address Range | Purpose |
|---|---|---|
| Exception vectors | `0x000000`–`0x000100` | SSP at 0, PC at 4 (PC slot holds `EXEC_BASE`), plus initial stack pointer. Autovectors 1–7 (`0x64`–`0x7C`) point at the IRQ dispatch stubs. |
| exec.library stubs | `0x000C1C`–`0x001000` | Catch-all `ILLEGAL` stubs for every LVO `-6`…`-996`, real dispatchers overlaid where implemented. |
| ExecBase structure | `0x001000`–`~0x001280` | Positive `struct ExecBase` image — list headers, `ThisTask`, `VBlankFrequency`/`PowerSupplyFrequency` (both 50). `EXEC_BASE = 0x1000`. |
| dos.library stubs | `0x001C1C`–`0x002000` | Same `-6`…`-996` catch-all band. `DOS_BASE = 0x2000`. |
| Other library bases | `0x003000`–`0x00F000` | bsdsocket `0x3000`, graphics `0x8000`, intuition `0x9000`, gadtools `0xA000`, audio.device `0xE000`, generic fake `0xF000` — each with its own negative stub band. |
| Loadable .library blobs | `0x00B000`–`0x01A000` | Raw `Workbench:LIBS/*.library` file images installed by `install_loadable_libs()`, 4 KB slots stepping `0x1000`, skipping the reserved `0xE000`/`0xF000` base pages. The band used to start at `0xA000` — the first blob's `UAOS` file header overwrote gadtools' Library node and corrupted `LibList` (UAOS-239). |
| Process environment | `0x01B000`–`0x01B400` | `UAOS_Emu_SetupProcess` block: `struct Process` (`0x1B000`), `CommandLineInterface` (`0x1B100`), ReadArgs scratch (`0x1B180`), console MsgPort (`0x1B1C0`), task-name C string + BSTRs for `cli_SetName`/`cli_Prompt`/current dir (`0x1B200`+). Dedicated region outside the program heap and every guest allocator (UAOS-237). |
| Hook return trap | `0x1EF000` | `ILLEGAL` marker a pushed hook ISR returns into (`UAOS_InvokeM68kHook`). |
| IRQ dispatch stubs | `0x1EF044`–`0x1EF05C` | Seven 4-byte `ILLEGAL` words tagged `(LIB_IRQ, level)` — the autovector entry points for hardware interrupt delivery (UAOS-241). |
| Program segments | `0x020000` up | Loaded Amiga Hunk code/data/BSS (`PROG_BASE = 0x20000`). |
| Stack | grows down from `0x1F0000` | Initial SP; `DOS_EXIT` stub (`0x1F70`) pushed as the top-level return address. |

`libmap_selfcheck()` runs once per `install_library_tables()` and logs `[emu] libmap selfcheck OK` — it asserts every library's negative stub band and positive struct stay inside guest RAM and pairwise-disjoint (UAOS-252; the old `EXEC_BASE=0x300`/`DOS_BASE=0x800` map let DOS stubs overwrite ExecBase fields).

## Guest Process Environment (UAOS-237)

`UAOS_Emu_SetupProcess()` populates a real `struct Process` environment after `hunk_load` — invoked identically by the shared-context path (`UAOS_Emu_LoadAndRun_Internal`) and the per-task path (`m68k_wrapper_entry`). The block lives at `0x1B000`, below `PROG_BASE` and outside every guest allocator, so large hunks can never overlap it (the original UAOS-237 failure: `FAKE_PROCESS_ADDR`/`FAKE_CLI_ADDR` sat inside the upward-growing heap and were stomped by big binaries).

Populated fields include: `ln_Type=NT_PROCESS`, `ln_Name` (command name as C string), `tc_State=TS_RUN`, stack bounds (`tc_SPLower`/`tc_SPUpper`, `pr_StackSize`, `pr_StackBase`), `pr_MsgPort` and `pr_ConsoleTask` as real NT_MSGPORT ports with allocated signal bits and `mp_SigTask` = the Process, `pr_CIS`/`pr_COS` (fake stdin/stdout BPTRs `0x101`/`0x100`), `pr_SegList` and `cli_Module`/`cli_Module`-SAS/C slot (real hunk seglist BPTR), `cli_CommandName`/`cli_CommandLine`/`cli_SetName`/`cli_Prompt`/`cli_CurrentDirName` BSTRs, `cli_DefaultInput`/`cli_DefaultOutput`, `cli_Interactive`, `pr_WindowPtr=-1`, empty `pr_LocalVars` MinList. `ExecBase+0x114` (`ThisTask`) points at the Process; `dos.library/GetConsoleTask`/`SetConsoleTask` mirror `pr_ConsoleTask` of the current task.

**BPTR alignment invariant**: every pointer handed to the guest as a BPTR must be 4-byte aligned — the launcher keeps `sp &= ~3` after each stack reservation. A 2-aligned `cmdline_ptr` cascaded into a misaligned command-name BSTR, so the BPTR round-trip lost the low bits and `SetupProcess` read a zero length byte → empty `ln_Name`.

Every guest-visible library base carries a full `struct Library` header (`ln_Type`, `ln_Name`→guest string, `lib_NegSize`/`lib_PosSize`, `lib_Version`/`lib_Revision`, `lib_IdString`, `lib_OpenCnt`) and is linked into the matching ExecBase list (`LibList`, `DeviceList`, `ResourceList`, `PortList`), so `FindName`/`OpenLibrary` version checks and `Forbid`-free list walks behave like real Exec. Unknown `OpenLibrary(name, ver)` materialises a generated base in the `0x7F0000`–`0x800000` guard band with `lib_Version = max(requested, 39)` — an appropriately versioned base rather than NULL (policy: permissive, keeps tracker-style binaries from dying on optional libs).

Note: the upper 16 MB address range also contains the Amiga custom chip/CIA window at `0xB00000`–`0xDFFFFF`.  Accesses to this range are not satisfied from the guest RAM array; instead, the Musashi memory callbacks in `uaos_m68k_glue.c` route them to the chipset emulator (`chip_emu_read`/`chip_emu_write`), using the same entry points as the native x86_64 page fault handler.  This allows M68k code to read and write Amiga hardware registers directly.

M68k tasks are given a private VBlank signal bit (`UaosTask.m68k_vblank_sig`) so that `graphics.library/WaitTOF()` can block on `Wait()` instead of busy-waiting.  The VBlank path in `timer_ProcessTicks()` signals the waiting task, keeping the idle/WM task responsive while M68k animations run at ~50 Hz.

Guest message ports (`exec.library` `PutMsg`/`GetMsg`/`ReplyMsg`/`WaitPort` in `uaos_m68k_glue.c`) carry real wakeup semantics: every poster queues the node on `mp_MsgList` and then `Signal()`s `mp_SigTask` with `mp_SigBit`.  `WaitPort` sleeps in bounded ~10 ms `Task_WaitTicks(sigbit, 1)` slices — a real blocking wait that deschedules immediately (UAOS-169) and wakes early when the port's signal arrives — capped at ~100 ms per call, instead of returning 0 instantly — an instant return made every IDCMP loop a 100%-CPU spin that died on the M68k cycle budget.  The cap preserves progress for ports that never receive messages (console/device reply ports polled by CLI tools).  Ports without `mp_SigTask` wired keep the old immediate return.  While blocked in the shared `UAOS_Emu_LoadAndRun` context (`g_chipset_sync_disabled == 0`), `WaitPort` re-pumps `UAOS_Intuition_PostIntuiTicks()` so `IDCMP_INTUITICKS` keeps flowing; per-task contexts never pump ticks (the slot table's `guest_win` pointers belong to other tasks' address spaces).

## Guest Interrupt Delivery (UAOS-241)

ExecBase `IntVects[16]` lives at `EXEC_BASE+0x54` as 12-byte `IntVector` entries. `SetIntVector` writes `iv_Data`/`iv_Code` (direct call); `AddIntServer` converts the slot to an `Interrupt`-node chain (`iv_Code = IV_CHAIN` sentinel, `iv_Node` = head) with last-unclaimed dispatch semantics; `RemIntServer` unlinks; `Cause` invokes the handler immediately. Handlers are entered with the Amiga ISR convention (D0 = INTREQ bit, A0 = `$DFF000` or CIA base, A1 = `is_Data`, A5 = `IntVector`, A6 = SysBase) via `m68k_isr_call()` — host-side context save, push the hook-return trap as return address, run bounded `m68k_execute` slices.

**Two delivery routes funnel through the same dispatch helpers** (`irq_pending_bits`, `deliver_intvect_bit`, `deliver_cia_icr`):

- **Real autovector** — `chip_emu_update_irq()` drives `m68k_set_irq()` for the live Musashi context. Each guest RAM window's autovectors 1–7 point at a `(LIB_IRQ, level)` ILLEGAL stub; Musashi pushes the 68020 format-0 exception frame and `irq_vector_entry()` runs the level's pending sources, then pops `[SR:w][PC:l][fmt:w]` — an emulated RTE back to the interrupted code. Diag: `g_dlv_vec`.
- **Poll path** — `UAOS_M68k_DeliverInterrupts()` is called between `m68k_execute` slices in `exec_task.c` and pumped inside the blocking-wait paths (`exec_Wait`, `WaitPort`, `WaitIO`, `dos_Delay` sliced sleeps, the ASL requester wait) so handlers also fire while the guest task is descheduled or while it runs at IPL 7. Diag: `g_dlv_calls`/`g_dlv_pend`/`g_dlv_isr`/`g_blocked_in`.

Both honour guest masking: `pend = INTREQ & INTENA` (plus the CIA-B line synthesized into the EXTER bit), and a source only preempts while its level exceeds the guest's `SR.IPL` (or reaches the vector path via Musashi's own level check). Unacknowledged bits persist in `g_intreq` and refire — matching level-triggered hardware.

**cia.resource** — `ciaa.resource`/`ciab.resource` open as generated-library blocks with an extra 8-entry `Interrupt*` table at `base+0x80`. `AddICRVector`(-6)/`RemICRVector`(-12)/`AbleICR`(-18)/`SetICR`(-24) dispatch to `chip_emu_cia_*` helpers; pending `icr & icr_mask` bits are delivered to the registered handlers (A0 = CIA base, A1 = `is_Data`, D0 = bit) and acked on delivery. GenLib entries are window-scoped (`GenLib.ram`) so concurrent M68k tasks keep independent resource registrations.

## Trap System

The emulation layer uses the `ILLEGAL` opcode to implement system calls (Traps). When the emulator encounters an `ILLEGAL` instruction, the glue logic checks the address to determine which LVO is being called. `TRAP #1` is used for simple DOS-style I/O (Write/Output, etc.).

## Embedded Binary Registry

`emulation/binaries/` holds Amiga binaries that are converted to C byte arrays by `scripts/embed_binary.sh`. The registry in `uaos_emu_registry.c` maps names (e.g., `Lha`) to the embedded data via `UAOS_Emu_RunByName()`. Since UAOS-75, `C:run` resolves commands through the normal shell dispatch path and spawns them detached; the registry remains only as a fallback when `run` is invoked without a shell dispatch context (e.g. a minimal `NativeCmdCtx`).

For details on how M68k code calls native functions, see [Thunking](/concepts/thunking.md).
