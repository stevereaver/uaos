---
type: External Resource
title: amiga_libs repository
description: Sister repo of ~1,480 AmigaOS .library binaries under reverse engineering; reconstructed sources and OKF bundles usable as inputs to UAOS.
resource: /../amiga_libs
tags: [external, libraries, m68k, reverse-engineering]
timestamp: 2026-10-14T06:42:45Z
---

# amiga_libs Repository

Sibling workspace at `/home/reaver/workspaces/amiga_libs` collecting AmigaOS
`.library` binaries for reverse engineering and source reconstruction
(~1,480 versions, ~650 distinct libraries; 680x0 CPU libs, xpk/XPK
sublibraries, xpr transfer protocols, datatype support, third-party libs).

## Layout

```
libraries/<name>_<ver>.library/
  binary/    # original binary (gitignored)
  source/    # reconstructed C/asm that rebuilds a real hunk .library
  okf/       # per-library knowledge bundle (index/log/functions/...)
  work/      # scratch (gitignored)
tools/RE_WORKFLOW.md   # pipeline spec: inspect -> reconstruct -> okf
```

Reconstructions build with `/opt/amiga` (m68k-amigaos-gcc + `ira`
disassembler) into genuine `HUNK_HEADER` (0x000003F3) `.library` binaries
with correct RomTag/InitTable. Many are oracle-verified against the
original binary.

## Reconstructed source ready today (excerpt)

- **Math**: mathieeedoubbas, mathieeesingtrans, mathieeedoubtrans
  (10.881 + 40.6 FPU variants), mathieeesingbas, mathtrans 40.6,
  mathffp 36.5
- **zlib_3.2**: full recovered zlib source (43 files)
- **iffparse 40.1**: complete exemplar, 63-file OKF bundle, per-LVO docs
- **XPK**: ~40 sublibraries (RLEN, NONE, FRLE, CBR0, DLTA, ILZR, SMPL,
  HFMN, NUKE, SQSH, MASH, LZW*, BLZW, ZENO, TDCS, FBR2, BZIP, ...)
- **version.library** (many versions), **_asl._40.6**, utility_41.0,
  rexxsupport 34.9, iff_16.4, plus misc small libs

## Relevance to UAOS

- **Done (UAOS-305)**: UAOS now loads real hunk `.library` files from
  `LIBS:` on first `OpenLibrary` (LoadSeg → RomTag → InitResident → Open
  vector). Reconstructed `version.library` 40.43 and `xpkNONE.library`
  1.5 from this repo were verified loading and serving vectors. See
  `okf/kernel/exec/other_libraries.md` "Disk-loaded libraries".
- Resolution order in `exec_OpenLibrary`: fixed bases -> disk-lib table
  -> ROM registry -> genlib cache -> LIBS: disk load -> declined list ->
  fake catch-all.
- UAOS lacks mathieeedoubbas/mathieeesingtrans/mathieeedoubtrans —
  reconstructed sources document exact semantics for native ports (Musashi
  FPU is disabled, so the binaries themselves would trap).
- Declined libs (amigaguide, powerpacker, lh, rexxsyslib) have binaries
  in amiga_libs but no reconstruction yet — RE backlog candidates whose
  completion directly un-declines UAOS features.
- iffparse's complete per-LVO documentation is a conformance oracle for
  `kernel/exec/iffparse_lib.c`.

Tracked in Plane: UAOS-304.
