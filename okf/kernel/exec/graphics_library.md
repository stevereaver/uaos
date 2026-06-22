---
type: Kernel Library
title: graphics.library
description: UAOS native implementation of the AmigaOS graphics.library for emulated M68k tasks.
resource: /kernel/exec/graphics_lib.c
tags: [graphics, library, m68k, thunking, rastport]
timestamp: 2026-06-22T15:00:00Z
---

# graphics.library

`graphics.library` provides the classic AmigaOS graphics API to emulated M68k tasks. The UAOS implementation is a native thunk layer that reads M68k registers and writes to either the host linear framebuffer (screen RastPort) or a planar Amiga-style `BitMap` attached to a `RastPort`.

## Key files

- `kernel/exec/graphics_lib.c` — native implementations and function table.
- `kernel/exec/amiga_graphics.h` — minimal RastPort/BitMap structure layouts.
- `emulation/uaos_m68k_glue.c` — M68k LVO stub installation and dispatch.

## Implementation status

### State accessors

| Function | Status | Notes |
|----------|--------|-------|
| `SetAPen` | Implemented | Writes `FgPen` in guest RastPort. |
| `SetBPen` | Implemented | Writes `BgPen` in guest RastPort. |
| `SetDrMd` | Implemented | Writes `DrawMode` in guest RastPort. |
| `GetAPen` | Implemented | Returns `FgPen` in D0. |
| `GetBPen` | Implemented | Returns `BgPen` in D0. |
| `GetDrMd` | Implemented | Returns `DrawMode` in D0. |
| `GetOutlinePen` | Implemented | Returns `OlPen` in D0. |
| `SetOutlinePen` | Implemented | Writes `OlPen`; used as outline colour when `OUTLINE` draw mode is set. |
| `SetWriteMask` | Implemented | Stores `Mask`; applied to all writes through a `RastPort` surface. |
| `SetMaxPen` | Implemented | Stores `MaxPen`; `FgPen`/`BgPen` are clamped to this value. |
| `SetABPenDrMd` | Implemented | Atomic FgPen/BgPen/DrawMode update. |
| `SetChipRev` | Implemented | Reports ECS revision (2). |
| `GetExtSpriteA` | Implemented | Returns 0 (no extended sprite data). |
| `SetFont` | Implemented | Sets `rp->Font` pointer; returns previous font. |
| `AskFont` | Implemented | Returns `rp->Font` pointer in D0. |

### View/ViewPort/BitMap initialisation

| Function | Status | Notes |
|----------|--------|-------|
| `InitView` | Implemented | Zeroes the guest `View` structure. |
| `InitVPort` | Implemented | Zeroes the guest `ViewPort` structure. |
| `InitBitMap` | Implemented | Sets `BytesPerRow`, `Rows`, `Depth`, `Flags`, and clears plane pointers. |
| `LoadView` | Implemented | Renders the first `ViewPort`'s planar `BitMap` into the host linear framebuffer using its `ColorMap`; `LoadView(NULL)` blanks the screen. |
| `ChangeVPBitMap` | Implemented | Swaps the `BitMap` pointer in the `ViewPort`'s `RasInfo` (or supplied `RasInfo`) without rebuilding the display. |
| `GetVPModeID` | Implemented | Returns `ViewPort.DisplayID` from A0. |
| `GetBitMapAttr` | Implemented | Returns `BMA_WIDTH/HEIGHT/DEPTH/FLAGS/BASE/ROWBYTES` from A0. |

### Hardware/viewport no-ops

| Function | Status | Notes |
|----------|--------|-------|
| `WaitBlit` | Implemented | No-op; host has no custom blitter. |
| `WaitBOVP` | Implemented | No-op; no beam sync hardware. |
| `VBeamPos` | Implemented | Returns 0. |
| `SetRGB4` | Implemented | Sets a `ViewPort` palette entry from 4-bit RGB components. |

### Drawing primitives

| Function | Status | Notes |
|----------|--------|-------|
| `InitRastPort` | Implemented | Zeroes RastPort and sets default pens. |
| `Move` / `Draw` | Implemented | Bresenham line drawing on the RastPort surface (planar BitMap or framebuffer). |
| `PolyDraw` | Implemented | Draws connected line segments from an XY array on the RastPort surface. |
| `RectFill` | Implemented | Filled rectangle on the RastPort surface; planar fills use per-plane byte masks. |
| `EraseRect` | Implemented | Fills rectangle with background pen on the RastPort surface. |
| `ClearEOL` | Implemented | Clears from pen position to right edge of the surface. |
| `ClearScreen` | Implemented | Clears the entire RastPort surface with background pen. |
| `Text` | Implemented | 8×16 font rendering on the RastPort surface; spacing and metrics come from the current `rp->Font`. |
| `TextLength` | Implemented | Returns pixel width based on current font character width. |
| `TextExtent` | Implemented | Fills `TextExtent` from the current font/metrics. |
| `TextFit` | Implemented | Returns characters that fit in a width constraint. |
| `FontExtent` | Implemented | Fills `TextExtent` from a font. |
| `SetRast` | Implemented | Fills the RastPort surface to the given pen. |
| `ReadPixel` / `WritePixel` | Implemented | Pixel read/write on the RastPort surface; returns/writes pen indices for planar BitMaps. |
| `DrawEllipse` | Implemented | Midpoint ellipse outline on the RastPort surface. |
| `AreaEllipse` | Implemented | Filled ellipse via horizontal scanlines on the RastPort surface. |
| `Flood` | Implemented | 4-way scanline flood fill from start point on the RastPort surface. |
| `BltClear` | Implemented | Zeroes a guest memory block. |
| `InitArea` / `AreaMove` / `AreaDraw` / `AreaEnd` | Implemented | Polygon path + scanline fill on the RastPort surface (internal state keyed by RastPort). |

### Font / text subsystem

| Function | Status | Notes |
|----------|--------|-------|
| `OpenFont` | Implemented | Matches a font by name/size/style and returns a `TextFont`. |
| `CloseFont` | Implemented | Decrements the matched font's reference count. |
| `SetFont` | Implemented | Sets `rp->Font` pointer; returns previous font. |
| `AskFont` | Implemented | Returns `rp->Font` pointer in D0. |
| `AddFont` | Implemented | No-op for already-known fonts. |
| `RemFont` | Implemented | No-op; static fonts are never removed. |
| `AskSoftStyle` | Implemented | Returns current `rp->SoftStyle`. |
| `SetSoftStyle` | Implemented | Updates `rp->SoftStyle` under enable mask; returns old style. |
| `ExtendFont` | Implemented | Returns TRUE for any known font. |
| `StripFont` | Implemented | No-op for the built-in font. |

### BitMap / RastPort allocation and management

| Function | Status | Notes |
|----------|--------|-------|
| `InitBitMap` | Implemented | Initialises a `BitMap` header and clears plane pointers. |
| `AllocBitMap` | Implemented | Allocates a `BitMap` plus `depth` planar bitplanes (`BytesPerRow = ((width+15)/16)*2`); honours `BMF_CLEAR`. |
| `FreeBitMap` | Implemented | Frees all planar bitplanes and the `BitMap` header. |
| `GetBitMap` | Implemented | Returns `rp->BitMap` pointer in D0 (internal helper). |
| `GetBitMapAttr` | Implemented | Returns `BMA_WIDTH/HEIGHT/DEPTH/FLAGS/BASE/ROWBYTES`; `BMA_WIDTH` = `BytesPerRow * 8`. |
| `AllocRaster` | Implemented | Allocates a planar raster buffer, zeroed. |
| `FreeRaster` | Implemented | Frees a raster buffer allocated by `AllocRaster`. |
| `InitTmpRas` | Implemented | Initialises a `TmpRas` structure with buffer and size. |
| `SetRast` | Implemented | Fills the RastPort's surface (`BitMap` or screen) with a pen. |

### Planar BitMap representation

UAOS now stores `BitMap`s in real Amiga planar format:

- `BytesPerRow` is the number of bytes per scanline in each plane, rounded to a 16-bit (2-byte) boundary: `((width + 15) / 16) * 2`.
- `Depth` bitplanes are allocated separately; `Planes[0..Depth-1]` point to each plane.
- A pixel at `(x, y)` is formed by combining bit `7 - (x % 8)` of byte `y * BytesPerRow + x/8` from each plane.
- `GetBitMapAttr(bm, BMA_WIDTH)` returns `BytesPerRow * 8` (the true pixel width of a planar BitMap).
- Drawing operations (`Draw`, `RectFill`, `Text`, `WritePixel`, etc.) and blitting (`BltBitMap`, `ClipBlt`, `BltBitMapRastPort`) work on the RastPort's surface, whether that is a planar BitMap or the screen framebuffer.
- When writing to a screen RastPort, pen indices are converted to 32-bit RGB using the default Amiga palette; when writing to a planar BitMap, pen indices are written directly into the bitplanes.
- Mixed planar↔framebuffer blits convert colours between pen indices and RGB as needed so the destination colour space is preserved.
- `LoadView` translates a planar `BitMap` into the linear host framebuffer, converting pen indices through the `ViewPort`'s `ColorMap` (or the default palette if none).

### Region operations

| Function | Status | Notes |
|----------|--------|-------|
| `NewRegion` | Implemented | Allocates an empty `Region` (single `RegionRectangle`). |
| `DisposeRegion` | Implemented | Frees a `Region` and its `RegionRectangle` list. |
| `AndRectRegion` | Implemented | Intersects region with a `Rectangle`; returns non-empty status. |
| `OrRectRegion` | Implemented | Unions region with a `Rectangle`; returns non-empty status. |
| `XorRectRegion` | Implemented | Approximated as union (single bounding-box representation). |
| `ClearRectRegion` | Implemented | Empties region if rectangle fully covers it; otherwise no-op. |
| `ClearRegion` | Implemented | Empties the region. |
| `AndRegionRegion` | Implemented | Intersects two regions; destination is updated. |
| `OrRegionRegion` | Implemented | Unions two regions; destination is updated. |
| `XorRegionRegion` | Implemented | Approximated as union (single bounding-box representation). |

### CPU-based blitting and scrolling

| Function | Status | Notes |
|----------|--------|-------|
| `BltBitMap` | Implemented | CPU copy from `BitMap` to `BitMap` with minterms and optional mask. |
| `BltTemplate` | Implemented | Renders a 1-bit template using `FgPen`/`BgPen` (JAM1/JAM2). |
| `BltPattern` | Implemented | Tiles a 16×16 1-bit pattern over a rectangle. |
| `ClipBlt` | Implemented | Blits between two `RastPort`s using the same blit engine. |
| `BltBitMapRastPort` | Implemented | Blits from a `BitMap` to a `RastPort`. |
| `BltMaskBitMapRastPort` | Implemented | Blits from a `BitMap` to a `RastPort` with a planar mask. |
| `ScrollRaster` | Implemented | Copies a rectangle by `(dx,dy)` without filling exposed area. |
| `ScrollRasterBF` | Implemented | Copies a rectangle by `(dx,dy)` and fills exposed area with `BgPen`. |

### ColorMap / palette state

| Function | Status | Notes |
|----------|--------|-------|
| `GetColorMap` | Implemented | Allocates a `ColorMap` with a 32-bit RGB colour table. |
| `FreeColorMap` | Implemented | Frees a `ColorMap`, its colour table, and any `PalExtra`. |
| `AttachPalExtra` | Implemented | Allocates a `PalExtra` (pen ref counts / alloc bitmap) for a `ColorMap`. |
| `SetRGB4` | Implemented | Sets a `ViewPort` palette entry from 4-bit RGB components. |
| `LoadRGB4` | Implemented | Loads a `ViewPort` palette from a packed RGB4 table. |
| `GetRGB4` | Implemented | Returns a `ViewPort` palette entry as packed RGB4. |
| `SetRGB4CM` | Implemented | Sets a `ColorMap` entry from 4-bit RGB components. |
| `SetRGB32` | Implemented | Sets a `ViewPort` palette entry from 32-bit RGB components. |
| `LoadRGB32` | Implemented | Loads a `ViewPort` palette from a 32-bit RGB table. |
| `GetRGB32` | Implemented | Reads a `ColorMap` entry as 32-bit RGB. |
| `SetRGB32CM` | Implemented | Sets a `ColorMap` entry from 32-bit RGB components. |
| `FindColor` | Implemented | Finds the closest `ColorMap` entry within a tolerance. |
| `ObtainPen` | Implemented | Obtains (or reuses) a `ColorMap` pen for a colour. |
| `ReleasePen` | Implemented | Releases a `ColorMap` pen obtained by `ObtainPen`. |
| `ObtainBestPenA` | Implemented | Returns the best matching pen in a `ColorMap`. |

### Display-mode database

| Function | Status | Notes |
|----------|--------|-------|
| `OpenMonitor` | Implemented | Returns a built-in `MonitorSpec` (dummy). |
| `CloseMonitor` | Implemented | No-op. |
| `FindDisplayInfo` | Implemented | Returns a `DisplayInfo` record for a known mode ID. |
| `NextDisplayInfo` | Implemented | Iterates the built-in mode ID table. |
| `GetDisplayInfoData` | Implemented | Returns `DTAG_DISP_DIM` (dimensions) or `DTAG_NAME` data. |
| `GetVPModeID` | Implemented | Returns `ViewPort.DisplayID`. |
| `ModeNotAvailable` | Implemented | Returns 0 for known modes, 1 otherwise. |
| `BestModeIDA` | Implemented | Returns the first known mode ID. |
| `CoerceMode` | Implemented | Returns the mode ID if known, else the first known mode. |
| `VideoControl` | Implemented | No-op (no hardware video control). |

### Layer / Intuition helpers

| Function | Status | Notes |
|----------|--------|-------|
| `LockLayerRom` | Implemented | No-op; returns success (UAOS has no real layer lock). |
| `UnlockLayerRom` | Implemented | No-op. |
| `AttemptLockLayerRom` | Implemented | No-op; returns success. |
| `SyncSBitMap` | Implemented | No-op (no super-bitmap state). |
| `CopySBitMap` | Implemented | No-op (no super-bitmap state). |

### Advanced graphics support

| Function | Status | Notes |
|----------|--------|-------|
| `GfxNew` | Implemented | Allocates a minimal graphics library node. |
| `GfxFree` | Implemented | Frees a node created by `GfxNew`. |
| `GfxAssociate` | Implemented | Stores a single association pointer in a node. |
| `GfxLookUp` | Implemented | Returns the associated pointer from a node. |
| `SetRPAttrsA` | Implemented | Sets `RastPort` attributes from a tag list (FgPen/BgPen/DrawMode/BitMap). |
| `GetRPAttrsA` | Implemented | Reads `RastPort` attributes into a tag list. |
| `CalcIVG` | Implemented | Returns 0 (not meaningful for UAOS display). |
| `AllocDBufInfo` | Implemented | Allocates a dummy `DBufInfo` structure. |
| `FreeDBufInfo` | Implemented | Frees a `DBufInfo` structure. |
| `WeightAMatch` | Implemented | Returns 0 (single font). |
| `BitMapScale` | Implemented | Nearest-neighbour scale from source to destination `BitMap`. |
| `ScalerDiv` | Implemented | Returns `(factor * numerator) / denominator`. |

### Chunky pixel I/O

| Function | Status | Notes |
|----------|--------|-------|
| `ReadPixelLine8` | Implemented | Reads a line of framebuffer pixels into an 8-bit chunky array (greyscale). |
| `WritePixelLine8` | Implemented | Writes a line of 8-bit chunky pen indices to the framebuffer. |
| `ReadPixelArray8` | Implemented | Reads a rectangular region into an 8-bit chunky array (greyscale). |
| `WritePixelArray8` | Implemented | Writes a rectangular 8-bit chunky pen array to the framebuffer. |
| `WriteChunkyPixels` | Implemented | Writes a rectangular 8-bit chunky pen array to the framebuffer. |

### Still stubbed

- `LoadView`, `WaitTOF` — no copper/display hardware.
- Palette/colourmap state is maintained, but no copper/hardware register loading is performed.

## LVO dispatch

M68k code calls `graphics.library` via negative offsets from `GRAPHICS_BASE`. The UAOS jump table now matches the real AmigaOS `graphics.library` LVO offsets from `-30` to `-1056` (slot `|LVO| / 6` = `5`..`176`). Every LVO slot is populated: implemented functions are dispatched to their native C handlers, and unimplemented slots are filled with a safe no-op stub.

### Memory layout

- `GRAPHICS_BASE` is `0x8000`; the LVO jump table occupies `0x7BE0`..`0x8000`.
- `INTUITION_BASE` is `0x9000` and loadable libraries start at `0xA000` to avoid collision.
- Each slot is 6 bytes. Unknown slots are pre-filled with `MOVEQ #0,D0` + `RTS` at install time; every slot then receives an `ILLEGAL` dispatch stub that forwards to `UAOS_Graphics_Dispatch(slot)`.

### Full LVO table

| Slot | LVO | Function | Status |
|------|-----|----------|--------|
| 5 | -30 | BltBitMap | Implemented |
| 6 | -36 | BltTemplate | Implemented |
| 7 | -42 | ClearEOL | Implemented |
| 8 | -48 | ClearScreen | Implemented |
| 9 | -54 | TextLength | Implemented |
| 10 | -60 | Text | Implemented |
| 11 | -66 | SetFont | Implemented |
| 12 | -72 | OpenFont | Implemented |
| 13 | -78 | CloseFont | Implemented |
| 14 | -84 | AskSoftStyle | Implemented |
| 15 | -90 | SetSoftStyle | Implemented |
| 16 | -96 | AddBob | Stub |
| 17 | -102 | AddVSprite | Stub |
| 18 | -108 | DoCollision | Stub |
| 19 | -114 | DrawGList | Stub |
| 20 | -120 | InitGels | Stub |
| 21 | -126 | InitMasks | Stub |
| 22 | -132 | RemIBob | Stub |
| 23 | -138 | RemVSprite | Stub |
| 24 | -144 | SetCollision | Stub |
| 25 | -150 | SortGList | Stub |
| 26 | -156 | AddAnimOb | Stub |
| 27 | -162 | Animate | Stub |
| 28 | -168 | GetGBuffers | Stub |
| 29 | -174 | InitGMasks | Stub |
| 30 | -180 | DrawEllipse | Implemented |
| 31 | -186 | AreaEllipse | Implemented |
| 32 | -192 | LoadRGB4 | Implemented |
| 33 | -198 | InitRastPort | Implemented |
| 34 | -204 | InitVPort | Implemented |
| 35 | -210 | MrgCop | Stub |
| 36 | -216 | MakeVPort | Stub |
| 37 | -222 | LoadView | Stub |
| 38 | -228 | WaitBlit | Implemented |
| 39 | -234 | SetRast | Implemented |
| 40 | -240 | Move | Implemented |
| 41 | -246 | Draw | Implemented |
| 42 | -252 | AreaMove | Implemented |
| 43 | -258 | AreaDraw | Implemented |
| 44 | -264 | AreaEnd | Implemented |
| 45 | -270 | WaitTOF | Stub |
| 46 | -276 | QBlit | Stub |
| 47 | -282 | InitArea | Implemented |
| 48 | -288 | SetRGB4 | Implemented |
| 49 | -294 | QBSBlit | Stub |
| 50 | -300 | BltClear | Implemented |
| 51 | -306 | RectFill | Implemented |
| 52 | -312 | BltPattern | Implemented |
| 53 | -318 | ReadPixel | Implemented |
| 54 | -324 | WritePixel | Implemented |
| 55 | -330 | Flood | Implemented |
| 56 | -336 | PolyDraw | Implemented |
| 57 | -342 | SetAPen | Implemented |
| 58 | -348 | SetBPen | Implemented |
| 59 | -354 | SetDrMd | Implemented |
| 60 | -360 | InitView | Implemented |
| 61 | -366 | CBump | Stub |
| 62 | -372 | CMove | Stub |
| 63 | -378 | CWait | Stub |
| 64 | -384 | VBeamPos | Implemented |
| 65 | -390 | InitBitMap | Implemented |
| 66 | -396 | ScrollRaster | Implemented |
| 67 | -402 | WaitBOVP | Implemented |
| 68 | -408 | GetSprite | Stub |
| 69 | -414 | FreeSprite | Stub |
| 70 | -420 | ChangeSprite | Stub |
| 71 | -426 | MoveSprite | Stub |
| 72 | -432 | LockLayerRom | Implemented |
| 73 | -438 | UnlockLayerRom | Implemented |
| 74 | -444 | SyncSBitMap | Implemented |
| 75 | -450 | CopySBitMap | Implemented |
| 76 | -456 | OwnBlitter | Stub |
| 77 | -462 | DisownBlitter | Stub |
| 78 | -468 | InitTmpRas | Implemented |
| 79 | -474 | AskFont | Implemented |
| 80 | -480 | AddFont | Implemented |
| 81 | -486 | RemFont | Implemented |
| 82 | -492 | AllocRaster | Implemented |
| 83 | -498 | FreeRaster | Implemented |
| 84 | -504 | AndRectRegion | Implemented |
| 85 | -510 | OrRectRegion | Implemented |
| 86 | -516 | NewRegion | Implemented |
| 87 | -522 | ClearRectRegion | Implemented |
| 88 | -528 | ClearRegion | Implemented |
| 89 | -534 | DisposeRegion | Implemented |
| 90 | -540 | FreeVPortCopLists | Stub |
| 91 | -546 | FreeCopList | Stub |
| 92 | -552 | ClipBlit | Implemented |
| 93 | -558 | XorRectRegion | Implemented |
| 94 | -564 | FreeCprList | Stub |
| 95 | -570 | GetColorMap | Implemented |
| 96 | -576 | FreeColorMap | Implemented |
| 97 | -582 | GetRGB4 | Implemented |
| 98 | -588 | ScrollVPort | Stub |
| 99 | -594 | UCopperListInit | Stub |
| 100 | -600 | FreeGBuffers | Stub |
| 101 | -606 | BltBitMapRastPort | Implemented |
| 102 | -612 | OrRegionRegion | Implemented |
| 103 | -618 | XorRegionRegion | Implemented |
| 104 | -624 | AndRegionRegion | Implemented |
| 105 | -630 | SetRGB4CM | Implemented |
| 106 | -636 | BltMaskBitMapRastPort | Implemented |
| 107 | -642 | *reserved* | No-op |
| 108 | -648 | *reserved* | No-op |
| 109 | -654 | AttemptLockLayerRom | Implemented |
| 110 | -660 | GfxNew | Implemented |
| 111 | -666 | GfxFree | Implemented |
| 112 | -672 | GfxAssociate | Implemented |
| 113 | -678 | BitMapScale | Implemented |
| 114 | -684 | ScalerDiv | Implemented |
| 115 | -690 | TextExtent | Implemented |
| 116 | -696 | TextFit | Implemented |
| 117 | -702 | GfxLookUp | Implemented |
| 118 | -708 | VideoControl | Implemented |
| 119 | -714 | OpenMonitor | Implemented |
| 120 | -720 | CloseMonitor | Implemented |
| 121 | -726 | FindDisplayInfo | Implemented |
| 122 | -732 | NextDisplayInfo | Implemented |
| 123 | -738 | *reserved* | No-op |
| 124 | -744 | *reserved* | No-op |
| 125 | -750 | *reserved* | No-op |
| 126 | -756 | GetDisplayInfoData | Implemented |
| 127 | -762 | FontExtent | Implemented |
| 128 | -768 | ReadPixelLine8 | Implemented |
| 129 | -774 | WritePixelLine8 | Implemented |
| 130 | -780 | ReadPixelArray8 | Implemented |
| 131 | -786 | WritePixelArray8 | Implemented |
| 132 | -792 | GetVPModeID | Implemented |
| 133 | -798 | ModeNotAvailable | Implemented |
| 134 | -804 | WeighTAMatch | Implemented |
| 135 | -810 | EraseRect | Implemented |
| 136 | -816 | ExtendFont | Implemented |
| 137 | -822 | StripFont | Implemented |
| 138 | -828 | CalcIVG | Implemented |
| 139 | -834 | AttachPalExtra | Implemented |
| 140 | -840 | ObtainBestPenA | Implemented |
| 141 | -846 | *reserved* | No-op |
| 142 | -852 | SetRGB32 | Implemented |
| 143 | -858 | GetAPen | Implemented |
| 144 | -864 | GetBPen | Implemented |
| 145 | -870 | GetDrMd | Implemented |
| 146 | -876 | GetOutlinePen | Implemented |
| 147 | -882 | LoadRGB32 | Implemented |
| 148 | -888 | SetChipRev | Implemented |
| 149 | -894 | SetABPenDrMd | Implemented |
| 150 | -900 | GetRGB32 | Implemented |
| 151 | -906 | *reserved* | No-op |
| 152 | -912 | *reserved* | No-op |
| 153 | -918 | AllocBitMap | Implemented |
| 154 | -924 | FreeBitMap | Implemented |
| 155 | -930 | GetExtSpriteA | Implemented |
| 156 | -936 | CoerceMode | Implemented |
| 157 | -942 | ChangeVPBitMap | Stub |
| 158 | -948 | ReleasePen | Implemented |
| 159 | -954 | ObtainPen | Implemented |
| 160 | -960 | GetBitMapAttr | Implemented |
| 161 | -966 | AllocDBufInfo | Implemented |
| 162 | -972 | FreeDBufInfo | Implemented |
| 163 | -978 | SetOutlinePen | Implemented |
| 164 | -984 | SetWriteMask | Implemented |
| 165 | -990 | SetMaxPen | Implemented |
| 166 | -996 | SetRGB32CM | Implemented |
| 167 | -1002 | ScrollRasterBF | Implemented |
| 168 | -1008 | FindColor | Implemented |
| 169 | -1014 | *reserved* | No-op |
| 170 | -1020 | AllocSpriteDataA | Stub |
| 171 | -1026 | ChangeExtSpriteA | Stub |
| 172 | -1032 | FreeSpriteData | Stub |
| 173 | -1038 | SetRPAttrsA | Implemented |
| 174 | -1044 | GetRPAttrsA | Implemented |
| 175 | -1050 | BestModeIDA | Implemented |
| 176 | -1056 | WriteChunkyPixels | Implemented |

### Function index encoding

`emulation/uaos_m68k_glue.c` encodes the LVO slot number as the `func_idx` byte in the `ILLEGAL` dispatch word. The dispatcher in `graphics_lib.c` receives the slot number and indexes the `graphics_funcs` array directly:

```c
void UAOS_Graphics_Dispatch(uint32_t fn)
{
    if (fn < GFX_SLOT_MIN || fn > GFX_SLOT_MAX) { ... }
    void (*fp)(void) = graphics_funcs[fn];
    if (fp) fp();
    else graphics_Unimplemented();
}
```

`graphics_funcs` is sized `GFX_SLOT_MAX + 1` (177 entries) and all unimplemented slots are either left `NULL` or explicitly set to `graphics_Unimplemented`, so any out-of-range or missing entry safely returns to the caller.
