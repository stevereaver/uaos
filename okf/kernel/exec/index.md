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
- **Userspace Execution**: Running Ring-3 native x86-64 ELF64 tasks with memory protection (MMU sandboxing), parent-child hierarchy, and per-task current working directory.
- **System Calls**: Software interrupt `INT 0x80` register-based dispatcher facilitating kernel services for userspace programs.
- **Memory Allocation**: Managing the system memory map and providing allocation services.  `AllocMem` routes `MEMF_CHIP` to the 0–8 MB chip RAM range and `MEMF_FAST` to the 8–16 MB fast RAM range.
- **IPC (Inter-Process Communication)**: Message ports and signals for communication between tasks (including `SIGF_CHILD` for parent notification).
- **Library Loading**: Dynamic loading of native and emulated libraries.
- **MMU Sandboxing**: 4-level paging and memory protection.

## Core Files

- `task.c`: Task creation (native, X64 user-space, and emulated M68k) and context switching logic.
- `exec_task.c`: AmigaOS-compatible `AddTask`/`FindTask`/`SetTaskPri` helpers for M68k tasks.
- `exec_signal.c`: Task lookup helpers for M68k guest process structures.
- `exec_ipc.c`: Message port and message passing implementation (`NewPort`, `PutMsg`, `GetMsg`, `WaitPort`, `ReplyMsg`).
- `syscall_dispatch.c`: Handling register-based system call routing and implementation for Ring-3 userspace programs.
- `mem_info.c` / `mem_info.h`: Kernel-exported memory query API (`Mem_GetInfo()`), backing both the resident `C:mem` command and the `SYSCALL_MEMINFO` (0x2D) syscall consumed by the on-disk `C:avail` userspace command.
- `elf64_loader.c`: ELF64 PIE/EXEC loader for native x86-64 userspace binaries.
- `loadable_lib.c`: Scans `Workbench:LIBS/` for loadable Amiga `.library` files and registers them with the emulation layer.
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
- [bsdsocket.library](bsdsocket_library.md) — BSD socket API mapped to the native TCP/IP stack.
- [Other Libraries & Devices](other_libraries.md) — `utility.library`, `mathffp.library`, `locale.library`, `ixemul.library`, `console.device`, `keyboard.device`, `timer.device`.

## Task Stack Alignment

The x86-64 SysV ABI requires the stack pointer to be 16-byte aligned *before* a `CALL` instruction, which means a function is entered with `%rsp` 8-byte misaligned (the return address pushed by `CALL` makes it 16-byte aligned). To preserve this invariant across context switches, the kernel stacks are aligned to 8-byte boundaries (not 16-byte), and the synthetic interrupt frames built by `Task_CreateNative()` and `Task_CreateX64()` are sized so that the `iretq` epilogue leaves the new task with the ABI-required 8-byte misaligned `%rsp`.

Key details:
- Per-task kernel stacks are declared with `__attribute__((aligned(8)))`.
- Synthetic frames are 176 bytes for Ring-3 tasks (X64 ELF64) and 160 bytes for Ring-0 native tasks, matching the `iretq` pop count.
- `isr_common` and `uaos_syscall_isr` use the same frame layout for both the interrupted task and the task being switched to; no padding is inserted into the synthetic frame, keeping the layout identical to a CPU-generated interrupt frame.

## X64 Syscall Dispatch

X64 userspace tasks communicate with the kernel via INT 0x80 syscalls (`syscall_dispatch.c`). Key syscalls include `read`, `write`, `open`, `close`, `exit`, `getargs`, `spawn`, `wait`, `alloc`, `getcwd`, `opendir`, `readdir`, `stat`, GUI window operations, and the filesystem metadata syscalls (`SYSCALL_MKDIR` through `SYSCALL_GETMOUNTNAME`, 0x20–0x2C).

### Memory Query API (`SYSCALL_MEMINFO`, 0x2D)

`sys_meminfo` fills a `struct uaos_meminfo` (kernel side: `struct UaosMemInfo` in `mem_info.h`) with a point-in-time snapshot of the live memory arenas. It is a thin wrapper over the in-kernel `Mem_GetInfo()` helper in `mem_info.c`, which gathers:

- **x86-64 userspace heap** — total/used/free from the ELF64 loader bump arena (`ELF64_HeapSize()` / `ELF64_HeapUsed()`). This arena backs ELF64 segment loading and `sys_alloc`; it is reclaimed only when no X64 tasks are alive (`ELF64_ReclaimHeap()`).
- **Emulated M68k guest RAM slots** — per-task RAM pool count from `Task_M68kSlotCount()` and the per-slot size (`GUEST_RAM_SIZE`).
- **Scheduler task table** — total/running/waiting counts from `Task_GetCounts()`.

The same `Mem_GetInfo()` helper is consumed directly by the resident `C:mem` command, so kernel and userspace memory reports stay consistent. The on-disk `C:avail` userspace command queries this API to render real memory statistics.

### stdin read (`sys_read`, fd=0)

`sys_read` blocks until a newline is received from the PS/2 keyboard. When no key is available, it executes `hlt` to halt the CPU until the next interrupt. The timer ISR (100 Hz) then fires, calls `Task_ScheduleFromIRQ()`, and switches to other tasks (shell, desktop/WM, network poll). When this task is scheduled again, it resumes from the `hlt` and re-checks for input.

### Trap Gate for Vector 0x80

The INT 0x80 syscall gate is configured as a **trap gate** (IDT type 0xEF), not an interrupt gate (0xEE). A trap gate does not clear IF on entry, so interrupts remain enabled during syscall handlers. This is essential: `sys_read` and `sys_readkey` loop waiting for keyboard input and rely on the timer ISR to preempt them. With an interrupt gate, the timer ISR could not fire during a blocking syscall, freezing the entire UI.

### CPU Exception Handling (ISR_Dispatch)

`ISR_Dispatch` in `irq/idt.c` handles all IDT vectors. For CPU exceptions (vectors 0-31) with no registered handler, it checks whether the faulting task is an X64 userspace task. If so, the task is killed via `Task_Exit()` (printing a diagnostic message first) and the scheduler picks the next runnable task. This prevents a single buggy userspace command (e.g. a GNU coreutils binary that triggers a GPF) from locking up the entire OS. Kernel-mode exceptions still halt the system as a fatal panic.

## M68k Integration

Exec provides the bridge for emulated M68k tasks, including "LVO" (Library Vector Offset) stubs that allow M68k code to call native C functions.

## Guest Memory Layout

The emulated M68k guest RAM is wired into the 4 GB guest physical window at offset `0x00000000`.  `GUEST_RAM_SIZE` is defined as 16 MB, split into:

- **Chip RAM**: `0x00000000–0x007F0000` (8 MB minus a 64 KB guard)
- **Fast RAM**: `0x00800000–0x00FF0000` (8 MB minus a 64 KB guard)

`exec.library` `AllocMem` honours the request flags:

- `MEMF_CHIP` / `MEMF_DMA` / `MEMF_24BITDMA` allocate from chip RAM.
- `MEMF_FAST` allocates from fast RAM.
- Unspecified / `MEMF_PUBLIC` allocations prefer fast RAM and fall back to chip RAM.

The Amiga custom chip / CIA register window at `0x00B00000–0x00DFFFFF` is mapped non-present; accesses from M68k code fault to the page fault handler and are forwarded to the chip emulator in `kernel/chipset/chip_emu.c`.
