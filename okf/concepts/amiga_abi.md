---
type: Concept
title: Amiga Guest ABI
description: The AmigaOS-compatible ABI layer — guest structure layouts, constants, and LVO conventions for M68k software.
tags: [m68k, emulation, abi, ndk, amigaos]
status: stable
generated: { by: agent:devin, at: 2026-10-20T02:18:00Z }
---

# Amiga Guest ABI

UAOS presents an AmigaOS-compatible binary interface to M68k guest code. Guest
structures are big-endian, guest pointers are 32-bit, and BCPL pointers (`BPTR`)
are `addr >> 2`. Host code must marshal every field explicitly — host C structs
are never shared with the guest directly.

## Canonical Reference

`kernel/exec/amiga_task.h` is the canonical offset/constant reference, verified
against the AmigaOS NDK 40.13 headers (UAOS-353). It documents:

- `struct Node`/`List`/`MinList`/`MsgPort`/`Task`/`Process`/`IORequest`/
  `IOStdReq`/`Library`/`ExecBase`/`SignalSemaphore`/`SemaphoreRequest`/
  `SemaphoreMessage` layouts
- `NT_*` node types, `TS_*` task states, `TF_*` task flags, `PA_*` port actions
- `MEMF_*` memory attributes
- Process environment block layout (`PROC_ENV_BASE` region) and the CLI block:
  canonical V36 fields at `0x00–0x3C`, UAOS-private extensions at `0x40–0x7F`

Duplicated definitions in `emulation/uaos_m68k_glue.c`, `kernel/exec/dos_lib.c`,
device sources, and `system/Demos/src/*.s` have historically drifted — prefer the
header values. `dos_lib.c` includes `amiga_task.h` directly; the glue keeps local
`*_G` aliases mirroring it.

## NDK-Verified Behavior (UAOS-353)

- Exec: PutMsg/ReplyMsg insert by `ln_Pri` (Enqueue) and honour the port's
  `PF_ACTION` (`PA_SIGNAL`/`PA_SOFTINT`/`PA_IGNORE`); AddPort/AddSemaphore
  Enqueue; `CreateIORequest` starts as `NT_UNKNOWN`; OpenDevice failure returns
  `IOERR_OPENFAIL` (-1); `tc_SigAlloc` reserves low 16 bits (`0xFFFF0000`);
  ExecBase carries `SoftVer`/`MaxLocMem`/`MaxExtMem`/`EClockFrequency`/
  `VBlankFrequency`/`PowerSupplyFreq`/`LastAlert`; AvailMem honours
  `MEMF_LARGEST`/`MEMF_TOTAL`; heap allocations are 8-byte aligned
  (`MEM_BLOCKSIZE`); signal mirroring maps host break (bit 8) to guest
  `SIGBREAKF_CTRL_C` (bit 12).
- DOS: `FileLock` is 20 bytes (`fl_Volume` at 16); `InfoData` is serialized in
  canonical field order and handlers read the buffer from `dp_Arg2`;
  `ExAllData.ed_Name`/`ed_Comment` are C-string pointers (not BSTR BPTRs);
  `LockDosList` uses real `LDF_*` bit flags and returns a BPTR; `NextDosEntry`
  returns an APTR; ACTION_/ERROR_ codes match `dos/dosextens.h`/`dos/dos.h`.
- Devices: `timer.device` uses device LVOs (BeginIO -30, AbortIO -36, AddTime
  -42, SubTime -48, CmpTime -54, ReadEClock -60, GetSysTime -66); TR_* requests
  honour unit semantics (MICROHZ/VBLANK/ECLOCK/wait units); `GetSysTime` returns
  seconds since the Amiga epoch (1978-01-01); `keyboard.device` commands are
  `KBD_READEVENT`=9 through `KBD_RESETHANDLERDONE`=13; `audio.device` uses
  `ADCMD_ALLOCATE`=32 with FREE–WAITCYCLE at 9–14.
- Hardware: `CLXCON` at $098; Paula blitter DMA gate bit $0040; Copper
  end-of-list INTREQ bit $0010; CIA-A owns the parallel port; CIA ICR bit 7 is
  interrupt status; AUDxVOL bit 6 forces volume 64.

## Intentional Divergences / Remaining Gaps

- Guest demo sources still hand-roll LVOs/constants; vendoring the NDK `.i`
  includes is the main maintainability opportunity.
- Host-side `MsgPort`/`Message`/`SignalSemaphore` structs in `dospacket.h` and
  `exec_ipc.c` deliberately differ from guest layouts (native-only objects).
- `intuition_lib.h` GM_HANDLEINPUT uses a UAOS-private message layout.
- Hardware gaps: several read-only custom registers (POTGOR, SERBYT, DENISEID,
  DSKDATR, HHPOSR, COPINS), CIA TOD/serial-shift/timer modes, AGA display
  registers, blitter completion IRQ, audio attach/looping details.
- Missing devices: trackdisk, input, gameport, serial.device, parallel.device,
  clipboard, printer, narrator, scsidisk/CD support, keymap resource.
- Semaphore shared/exclusive modes and `SemaphoreMessage`-based
  Procure/Vacate semantics are not implemented (exclusive nesting only).
