---
type: Kernel Library
title: iffparse.library (IFF FORM/chunk parser)
description: Native iffparse.library v39 for M68k guests — FORM/LIST/CAT/PROP parsing, SCAN/STEP/RAWSTEP modes, entry/exit hooks, stored properties, collections, local context items, DOS and custom stream hooks.
resource: /kernel/exec/iffparse_lib.c
tags: [iffparse, iff, 8svx, m68k, thunking]
timestamp: 2026-10-06T12:30:00Z
---

# iffparse.library (`kernel/exec/iffparse_lib.c`)

AmigaOS iffparse.library (registered as v39, 40 vectors) for emulated M68k
guests. Implemented to the real NDK FD numbering (`AllocIFF` = -30 …
`IDtoStr` = -270); semantics follow the AROS parser FSM.

## Dispatch

`LIB_IFFPARSE = 11` in `emulation/uaos_m68k_glue.c`. `emu_gen_lib_base()`
recognises `"iffparse.library"` and installs the real LVO→fn map
(`g_iffparse_lvo_map`); all other LVOs get LIB_GENERIC catch-alls.
The ILLEGAL callback routes LIB_IFFPARSE through
`UAOS_ROM_NativeFunc("iffparse.library", fn)` — same mechanism as
dos/utility. Module registered from `UAOS_ROM_RegisterAll()`.

## Guest-visible structures (all in guest RAM, big-endian)

* `IFFHandle` (100 B): stream ptr, flags, context-stack head, FSM state,
  stream hook ptr (or builtin tag), 12 B stream-cmd scratch, header
  scratch, embedded default `IntContextNode`.
* `IntContextNode` (40 B): id/type/size/scan + singly-linked LCI list.
* `IntLocalContextItem` (36 B): id/type/ident, purge hook, user data.

Guests only ever navigate these through `CurrentChunk`/`ParentChunk` —
the list internals stay private.

## Implemented functions

| Group | Functions |
|---|---|
| Lifecycle | `AllocIFF`, `InitIFF` (guest hook), `InitIFFasDOS` (builtin DOS stream), `InitIFFasClip`/`OpenClipboard`/`CloseClipboard` (IFFERR_CLIPBOARD stubs), `OpenIFF`, `CloseIFF`, `FreeIFF` |
| Parser | `ParseIFF` (SCAN=0/STEP=1/RAWSTEP=2), `CurrentChunk`, `ParentChunk`, `PushChunk`, `PopChunk` (write: unknown-size backpatch + odd-pad + parent scan accounting) |
| Chunk I/O | `ReadChunkBytes`, `WriteChunkBytes`, `ReadChunkRecords`, `WriteChunkRecords` |
| Handlers | `EntryHandler`, `ExitHandler`, `StopChunk`, `StopChunks`, `StopOnExit` — guest `Hook`s invoked via `UAOS_InvokeM68kHook` (CallHookPkt: A0=hook, A1=msg, A2=object) |
| Properties | `PropChunk(s)`, `PropHooks`, `FindProp`, `FindPropContext` — payload stored as `StoredProperty` LCI in the enclosing FORM/LIST scope |
| Collections | `CollectionChunk(s)`, `CollectionHooks`, `FindCollection` — shared per-scope LCI, items prepended |
| Local items | `AllocLocalItem`, `LocalItemData`, `SetLocalItemPurge`, `FreeLocalItem`, `StoreLocalItem` (IFFSLI_ROOT/TOP/PROP), `StoreItemInContext`, `FindLocalItem` |
| Misc | `GoodID`, `GoodType`, `IDtoStr` |

## Parser FSM

States: COMPOSITE → PUSHCHUNK (read header) → ATOMIC → SCANEXIT → EXIT →
POPCHUNK (seek remainder + parent scan credit). Entry handlers run on
composite entry and on every atomic chunk; exit handlers on context pop.
`IFF_RETURN2CLIENT` (512) from a hook ends the call with return 0.
`IFFERR_EOC` (-3) ends a STEP/RAWSTEP context; `IFFERR_EOF` (-2) at the
default node. Leaf chunks inherit `cn_Type` from the containing node;
composite ckSize includes the 4-byte type longword.

`InitIFFasDOS` installs a builtin stream hook (`HandleTable_GetFile` +
`VFS_Read/Write/Seek`); IFFCMD_SEEK is `pos+offset`-relative; CLEANUP
rewinds to 0. Forward-only streams emulate seeks by discard-reading.

## Verification

* `SYS:Demos/IFFTest` ("IFFTEST PASS"): writes a FORM 8SVX to RAM: via
  PushChunk(IFFSIZE_UNKNOWN)/WriteChunkBytes/PopChunk size-backpatch,
  re-opens it with InitIFFasDOS/OpenIFF, scans with PropChunk('VHDR') +
  StopChunk('BODY') + FindProp + ReadChunkBytes, and walks it RAWSTEP —
  exact return sequence 0/-3/-2.
* OctaMED V5 acceptance (`tests/qemu_octamed_iff_test.py`): opens
  iffparse.library v37 at startup, loads OCTAMED:TEST.8SVX as an
  instrument with **zero unimplemented-LVO hits**. Measured caveat:
  strace stats show OctaMED issues **zero lib=11 calls** on instrument
  load/save and module load — it parses IFF with built-in code, so
  iffparse stays unexercised by OctaMED itself; the library's runtime
  proof is IFFTest plus real-app dispatch.
