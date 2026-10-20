---
type: Concept
title: Amiga Guest ABI
description: The AmigaOS-compatible ABI layer — guest structure layouts, constants, and LVO conventions for M68k software.
tags: [m68k, emulation, abi, ndk, amigaos]
status: stable
generated: { by: agent:devin, at: 2026-10-20T10:44:49Z }
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

## BOOPSI Method ABI (UAOS-362)

All guest-visible BOOPSI method IDs and message layouts are the real NDK
values — NDK-compiled guest classes interoperate with the built-in classes in
both directions (`DoSuperMethodA` into a native superclass, host-built messages
into a guest dispatcher):

- `OM_*` = `0x100`–`0x10A` (`intuition/classusr.h`): `opSet`, `opGet`,
  `opUpdate`, `opAddTail`, `opMember` layouts match.
- `GM_*` = `0`–`7` (`intuition/gadgetclass.h`, GM_DOMAIN is V47): `gpHitTest`,
  `gpRender`, `gpInput`, `gpGoInactive`, `gpLayout`, `gpDomain` (embedded IBox
  at `gpd_Domain`, `gpd_Which`/`gpd_Attrs`) layouts match. `GMR_*` return flags
  are the real bit values (`GMR_MEACTIVE`=0, `GMR_NOREUSE`=2, `GMR_REUSE`=4,
  `GMR_VERIFY`=8, `GMR_NEXTACTIVE`=16, `GMR_PREVACTIVE`=32,
  `GMR_GADGETHIT`=4).
- `IM_*` = `0x202`–`0x209` (`intuition/imageclass.h`): `impDraw`, `impErase`,
  `impHitTest` layouts match (`imp_Offset` at +8, `imp_State` at +12,
  `imp_DrInfo` at +16; `imp_Dimensions` carries the frame box for the
  `*FRAME` variants — no `IBox*` pointer).
- `gpi_IEvent` carries a real `struct InputEvent` (`devices/inputevent.i`):
  `ie_Class`/`ie_SubClass` are UBYTEs at +4/+5, `ie_Code` UWORD at +6,
  `ie_Qualifier` at +8, `ie_position` at +10/+12, `ie_TimeStamp` at +14
  (22 bytes total). `IECLASS_RAWKEY`=0x01; `IECODE_UP_PREFIX`=0x80 marks
  releases.

UAOS-private extensions that live *past the NDK struct end* (safe — NDK
classes never read them): `gpHitTest`+12 flags, `gpInput`+24 flags, `SGA_*`
action codes in bits 8+ of `strgclass` GM_* return values
(`SGA_END`=0x8000), and `IM_SIZECHANGE`/`IM_DOMAINFRAME`/`IM_SETDRWIDTH` at
`0x500+` (no NDK counterpart).

## Intentional Divergences / Remaining Gaps

- Guest demo sources still hand-roll LVOs/constants; vendoring the NDK `.i`
  includes is the main maintainability opportunity.
- Host-side `MsgPort`/`Message`/`SignalSemaphore` structs in `dospacket.h` and
  `exec_ipc.c` deliberately differ from guest layouts (native-only objects).
- BOOPSI guest-visible ABI is fully NDK-aligned (UAOS-362): method IDs,
  `gpInput`/`gpDomain`/`impDraw`/`impHitTest` layouts, `GMR_*` flags, and the
  `InputEvent` carried by `gpi_IEvent`.  Only trailing-field extensions and
  the `SGA_*`/`IM_*` private ranges remain UAOS-specific.
- Hardware gaps closed (UAOS-359/360): read-only custom registers (POTGOR,
  SERBYT, DENISEID, DSKDATR, HHPOSR, COPINS), CIA TOD/serial-shift/timer
  modes/pin semantics, AGA display registers, blitter completion IRQ,
  audio attach/looping details are now modelled.
- Missing devices: trackdisk, input, gameport, serial.device, parallel.device,
  clipboard, printer, narrator, scsidisk/CD support, keymap resource.
- Semaphores are now full V39 semantics (UAOS-361): shared/exclusive
  grants via `ss_QueueCount`, `ss_WaitQueue` mixing blocking waiters with
  `SemaphoreMessage` bids (`ln_Name` = SM_SHARED/SM_EXCLUSIVE),
  Procure/Vacate bid cancellation, `ProcureSegList` (V47 LVO -792) as a
  UAOS extension, and ExecBase completeness (ResModules, trap/exit-code
  stubs, SoftInt headers, `LastAlert[0..3]`, `TaskSigAlloc`/`TaskTrapAlloc`
  mirrors).  Multi-owner tracking is still single-`ss_Owner` like real
  Exec; blocked waiters park as `SUBW_SEM` contexts with a ~5 s
  stale-owner steal.
