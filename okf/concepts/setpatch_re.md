---
type: OKF Concept
title: SetPatch 43.6b reverse engineering
description: Byte-identical reassemblable reconstruction of Heinz Wrobel's Amiga SetPatch 43.6b, its module format, and the FS-UAE runtime harness. Lives in the untracked setpatch/ directory (Plane: UAOS-364).
resource: /okf/concepts/setpatch_re.md
tags: [amiga, reverse-engineering, setpatch, hunk, fs-uae]
timestamp: 2026-10-20T03:50:00Z
---

# SetPatch 43.6b reverse engineering

All artifacts live under `setpatch/` in the repo root — **untracked and
gitignored**; deliberately not integrated into UAOS (guest
SetFunction/semaphore state is per-task, so a guest SetPatch cannot
persist patches across tasks).

## Artifacts

- `original/SetPatch43_6b/SetPatch` — original Hunk executable
  (sha256 `edf32d1c6c6e1770f843f76d617585812438cf73cbe0538ba65971fc11a69d53`).
  Layout: HUNK_HEADER; hunk0 CODE 3384 longs (13536 B); hunk1 DATA 36
  longs (144 B); RELOC32 = 270 hunk0->hunk0 + 8 hunk0->hunk1.
- `tools/hunk2elf.py` — parses hunks, applies relocs (hunk0 @ 0x1000,
  hunk1 @ 0x4500), emits `re/code.bin`, `re/data.bin`, `re/relocs.txt`.
- `re/SetPatch.objdump.txt`, `decompiled/`, `re/modules.json`,
  `re/entries.txt` — disassembly, decompilations, module metadata.
- `tools/gen_source.py` — generates `src/SetPatch_43.6b.s` (4413 lines,
  GAS syntax). All 278 relocated longwords are symbolic label refs;
  38 modules labeled `mod_*`; op lists emitted as
  `dc.l $0BAD00nn,<label>` with comments. ~80 instruction sites are
  forced raw `dc.w`/`dc.b` because `m68k-amigaos-as` cannot reproduce
  the original encodings (esp. fpu/020+ and multiword movem forms).
- `tools/obj2hunk.py` — links the `m68k-amigaos-as` object straight to a
  Hunk executable, reproducing the original relocation group order from
  `re/relocs.txt` (as-produced order, not the sorted order GNU ld emits).
- `Makefile` — `make` builds `build/SetPatch_43.6b`;
  `make verify` proves `cmp` byte-identity + sha256 match;
  `make orig` rebuilds from `src/`.

## Module format (SetPatch patch modules)

Each module in hunk0 after the main driver (0x1000-0x16e8) and string
table (0x16ea-0x1866) starts with `0x6100FFFE` (bra.s) + a relocated
self-pointer, then a list of `(0x0BAD00nn, arg)` longword op pairs
terminated by 0 or op 13, followed by module code/data.

Ops: 0=name, 1=OpenLibrary, 2=FindName DeviceList, 3=OpenResource,
4=min ver/rev, 5=max ver, 6=call check fn, 7=set base literal,
8=block start, 9=block end, 10=new fn entry, 11=old-vector slot,
12=SetFunction LVO (copy block to AllocMem, SetFunction, stash old
vector), 13=final call/name-only install, 14=post-list finaliser,
15=open library only if already in LibList.

The installed semaphore is `"\xab SetPatch \xbb"`; `sps_Version`,
`sps_Revision`, and `sps_Private` is a MinList of patch nodes whose
inline name sits at node+8.

## FS-UAE harness (`test/`)

`test/run_fsuae.sh` builds a bootable ADF per scenario (`xdftool` OFS +
`boot install` + generated `s/startup-sequence`), mounts `test/dh0` as
DH0:, runs SetPatch twice + `checksp`, and kills FS-UAE on a `done`
sentinel. Scenarios: `run`, `quiet`, `nocache`, `stock` (WB3.1 SetPatch
40.16), `grab`. Docs: `test/README.md`.

### Kickstart 3.1 ROM quirk found

`~/fsuae/kick31.rom` (exec 40.10 + scsidisk 40.20) boots only on models
with motherboard/CPU-board RAM: exec relocates its exception vectors to
`0x08000398` (A3000/A4000 region); a bare A1200/A500 lacks RAM there and
crashes, and without an FPU it loops forever in the exec FPU probe at
`0xF80CA8` (`fmovel d1,fpcr`) because the F-line vector points at the
probe's own `rts` epilogue. Working models: `A3000` (68030+68882) and
`A4000/040`. A1200+Blizzard-1230-IV would need the absent Blizzard ROM.

`extras/WORKBENCH3.1_Install_save.adf` is a sparse "UAE-1ADF" save image
(no bootblock/filesystem — only 7 of 166 tracks have data), so the stock
SetPatch 40.16 came from a real `WB31.ADF` instead.

## Installed patch lists on Kickstart 3.1

Original 43.6b, A3000/68030 (7):

1. BOOL CheckIO() workaround
2. Enabled Advanced Graphics Modes
3. AmigaOS Copyright
4. Graphics WritePixelLine8
5. graphics/WriteChunkyPixel() patched
6. Memory Pools
7. Enabled data cache

A4000/040 adds `HWG IEEE Single Precision Mul/Div/FPU Fix` (8).
`NOCACHE` drops `Enabled data cache`; `QUIET` installs silently; a
second run reports "already been installed".

Stock WB3.1 SetPatch 40.16 (both models, 4): Enabled Advanced Graphics
Modes, WriteChunkyPixel, Memory Pools, Enabled data cache.

## SetPatch_UAOS_43.7 (task complete, UAOS-364 Done)

`src/SetPatch_UAOS_43.7.s` -> `build/SetPatch_UAOS_43.7` (15056 B).
Nine findings fixed (docs/BUGS.md has per-fix verification status):

- B1 op12 stale-oldslot double relocation -> wild write (runtime-proven
  both directions by `test/checksp_b1`)
- B2 op7 double CloseLibrary (dead code in shipped modules —
  inspection only)
- B3 SetFunction raced under Forbid (now Disable/Enable)
- B4 WBStartup launch ignored -> NULL pr_CLI deref + no reply
  (runtime-proven: `wb7` replies and installs; `wbold` stalls)
- B5 unbounded NMI RTE scan (bounded; module unreachable on KS3.1)
- B6 Line-A module returned garbage -> nondeterministic install
- B7 AGA module false-positived on ECS + used stale intuition base
  (proven: absent on A3000, present on A4000/040)
- B8 version identity -> `$VER: setpatch_uaos 43.7`
- B9a/b/c rebuild hazards: raw dispatcher word offsets -> computed
  `dc.w opN_hdl-(jtbl+2)`; op0 raw `braw` displacement -> computed
  `braw loc_14b0`; module-scan 4-byte stride -> all `$6100FFFE`
  headers must stay mod-4 aligned (nop pads on 2-byte inserts).
  B9c was the root cause of every flaky partial patch list.

Runtime matrix on FS-UAE Kick 3.1: A3000 6 patches, A4000/040 8,
QUIET/NOCACHE honored, second run "already installed". B2/B5 honestly
flagged inspection-only in docs/BUGS.md.

## UAOS-365: findings swept into UAOS (Done)

The driver's guest-visible requirements were audited against UAOS;
verified-already-correct: LoadSeg-style `[size][link][data]` hunk
layout, BSS zero-fill, `pr_SegList`/`cli_Module` publication, WBStartup
delivery (`pr_CLI=0`, queued message with `sm_Segment`, reply path),
`exec_CacheControl` returning 0. Real gaps fixed:

- **`exec_SetFunction` old vector** — first patch of an ILLEGAL stub
  slot returned the slot address, but the patch then overwrites the
  slot with `JMP new`, so chaining to "old" recursed into the
  replacement. Now returns a generated trampoline in `0xB000–0xC000`
  (stub copy + rts; inside `is_stub_addr`'s dispatch band so it still
  decodes by its lib/fn tag). Arena counter resets in
  `emu_reset_genlibs`.
- **Cross-window named semaphores** — SemaphoreList is per guest RAM
  window, but AmigaOS treats named semaphores as system-global
  (`« SetPatch »`). Host-side registry `g_semreg` records
  name+owning-window on `AddSemaphore`; `FindSemaphore` materializes a
  local replica (repaired linkage, remapped `ln_Name`, private
  MinList/patch-node copy) for cross-window hits;
  `UAOS_SemReg_ReleaseWindow` snapshots each registered sem's block +
  private nodes before the window is freed so replicas work even after
  the installer exits (AmigaOS abandoned-semaphore semantics).
- **`dos_ReadArgs`** — was a stub returning 1. Now a real subset
  parser (`/K` `/N` `/S` `/A` qualifiers, positional + keyword match)
  returning a real `RDArgs`. `dos_FreeArgs` frees it; `dos_GetArgStr`
  returns the launcher BPTR `g_cmdline_bptr` instead of 0.
- **`dos_PrintFault`** — DOS fn 104 / LVO `-798` was unimplemented;
  now dispatches sharing the extracted `dos_fault_text()` table with
  `dos_Fault`.
- **Bump allocator vs chip free list** — glue `heap_alloc` kept
  bumping past the free-list frontier once `g_heap_live`; now delegates
  to `dos_HeapAllocChip_glue` so internal allocations can't collide
  with freelist-owned memory.
- **`MEMF_REVERSE`** — honored by carving allocations from the high
  end of a suitable free block; propagated through the chip/fast pool
  wrappers and `mc_alloc`.

Regression: `system/Demos/src/SemPatchTest.s` +
`SemPatchRead.s` driven by `tests/qemu_sempatch_test.py` — 11/11
checks (SetFunction chain, cross-window FindSemaphore both
directions, ReadArgs, PrintFault, MEMF_REVERSE). No regressions in
`qemu_exec_coverage`, `qemu_guesttask`, or `qemu_wblaunch` suites.
Guests report via `RawPutChar` (PutStr goes to the shell window, not
serial).
