---
type: Kernel Library
title: graphics.library
description: UAOS native implementation of the AmigaOS graphics.library for emulated M68k tasks.
resource: /kernel/exec/graphics_lib.c
tags: [graphics, library, m68k, thunking, rastport]
timestamp: 2026-06-21T12:00:00Z
---

# graphics.library

`graphics.library` provides the classic AmigaOS graphics API to emulated M68k tasks. The UAOS implementation is a thin, native thunk layer that reads M68k registers and writes to the linear framebuffer via the internal display driver.

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
| `SetOutlinePen` | Implemented | Writes `OlPen`, returns old pen in D0. |
| `SetWriteMask` | Implemented | Stores `Mask` in guest RastPort; currently no effect on drawing. |
| `SetMaxPen` | Implemented | Stores `MaxPen` in guest RastPort; no effect on drawing. |
| `SetABPenDrMd` | Implemented | Atomic FgPen/BgPen/DrawMode update. |
| `SetFont` | Implemented | Sets `rp->Font` pointer; returns previous font. |
| `AskFont` | Implemented | Returns `rp->Font` pointer in D0. |

### View/ViewPort/BitMap initialisation

| Function | Status | Notes |
|----------|--------|-------|
| `InitView` | Implemented | Zeroes the guest `View` structure. |
| `InitVPort` | Implemented | Zeroes the guest `ViewPort` structure. |
| `InitBitMap` | Implemented | Sets `BytesPerRow`, `Rows`, `Depth`, `Flags`, and clears plane pointers. |
| `GetVPModeID` | Implemented | Returns `ViewPort.DisplayID` from A0. |
| `GetBitMapAttr` | Implemented | Returns `BMA_WIDTH/HEIGHT/DEPTH/FLAGS/BASE/ROWBYTES` from A0. |

### Hardware/viewport no-ops

| Function | Status | Notes |
|----------|--------|-------|
| `WaitBlit` | Implemented | No-op; host has no custom blitter. |
| `WaitBOVP` | Implemented | No-op; no beam sync hardware. |
| `VBeamPos` | Implemented | Returns 0. |
| `SetRGB4` | Implemented | No-op; fixed 32-bit palette. |

### Drawing primitives

| Function | Status | Notes |
|----------|--------|-------|
| `InitRastPort` | Implemented | Zeroes RastPort and sets default pens. |
| `Move` / `Draw` | Implemented | Bresenham line drawing via `FB_PutPixel`. |
| `PolyDraw` | Implemented | Draws connected line segments from an XY array. |
| `RectFill` | Implemented | Filled rectangle via `FB_FillRect`. |
| `EraseRect` | Implemented | Fills rectangle with background pen. |
| `ClearEOL` | Implemented | Clears from pen position to right edge of line. |
| `ClearScreen` | Implemented | Clears entire screen with background pen. |
| `Text` | Implemented | 8×16 font rendering. |
| `TextLength` | Implemented | Returns pixel width. |
| `SetRast` | Implemented | Clears screen to background pen. |
| `ReadPixel` / `WritePixel` | Implemented | Framebuffer pixel read/write. |
| `DrawEllipse` | Implemented | Midpoint ellipse outline via `FB_PutPixel`. |
| `AreaEllipse` | Implemented | Filled ellipse via horizontal scanlines. |
| `Flood` | Implemented | 4-way scanline flood fill from start point. |
| `BltClear` | Implemented | Zeroes a guest memory block. |
| `InitArea` / `AreaMove` / `AreaDraw` / `AreaEnd` | Implemented | Polygon path + scanline fill (internal state keyed by RastPort). |

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
- `BltBitMap`, `BltTemplate`, `BltPattern`, `BltBitMapRastPort`, `BltMaskBitMapRastPort` — complex blitter/memory operations; not yet implemented.
- `AllocBitMap`, `FreeBitMap` — no off-screen bitmap allocation yet.
- `LoadRGB4`, `LoadRGB32`, `GetColorMap`, `FreeColorMap`, `SetRGB32CM`, `GetRGB32` — palette/colourmap not wired to framebuffer.

## LVO dispatch

M68k code calls `graphics.library` via negative offsets from `GRAPHICS_BASE`. The UAOS jump table now matches the real AmigaOS `graphics.library` LVO offsets from `-30` to `-1056` (slot `|LVO| / 6` = `5`..`176`). Every LVO slot is populated: implemented functions are dispatched to their native C handlers, and unimplemented slots are filled with a safe no-op stub.

### Memory layout

- `GRAPHICS_BASE` is `0x8000`; the LVO jump table occupies `0x7BE0`..`0x8000`.
- `INTUITION_BASE` is `0x9000` and loadable libraries start at `0xA000` to avoid collision.
- Each slot is 6 bytes. Unknown slots are pre-filled with `MOVEQ #0,D0` + `RTS` at install time; every slot then receives an `ILLEGAL` dispatch stub that forwards to `UAOS_Graphics_Dispatch(slot)`.

### Full LVO table

| Slot | LVO | Function | Status |
|------|-----|----------|--------|
| 5 | -30 | BltBitMap | Stub |
| 6 | -36 | BltTemplate | Stub |
| 7 | -42 | ClearEOL | Implemented |
| 8 | -48 | ClearScreen | Implemented |
| 9 | -54 | TextLength | Implemented |
| 10 | -60 | Text | Implemented |
| 11 | -66 | SetFont | Implemented |
| 12 | -72 | OpenFont | Stub |
| 13 | -78 | CloseFont | Stub |
| 14 | -84 | AskSoftStyle | Stub |
| 15 | -90 | SetSoftStyle | Stub |
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
| 32 | -192 | LoadRGB4 | Stub |
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
| 52 | -312 | BltPattern | Stub |
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
| 66 | -396 | ScrollRaster | Stub |
| 67 | -402 | WaitBOVP | Implemented |
| 68 | -408 | GetSprite | Stub |
| 69 | -414 | FreeSprite | Stub |
| 70 | -420 | ChangeSprite | Stub |
| 71 | -426 | MoveSprite | Stub |
| 72 | -432 | LockLayerRom | Stub |
| 73 | -438 | UnlockLayerRom | Stub |
| 74 | -444 | SyncSBitMap | Stub |
| 75 | -450 | CopySBitMap | Stub |
| 76 | -456 | OwnBlitter | Stub |
| 77 | -462 | DisownBlitter | Stub |
| 78 | -468 | InitTmpRas | Stub |
| 79 | -474 | AskFont | Implemented |
| 80 | -480 | AddFont | Stub |
| 81 | -486 | RemFont | Stub |
| 82 | -492 | AllocRaster | Stub |
| 83 | -498 | FreeRaster | Stub |
| 84 | -504 | AndRectRegion | Stub |
| 85 | -510 | OrRectRegion | Stub |
| 86 | -516 | NewRegion | Stub |
| 87 | -522 | ClearRectRegion | Stub |
| 88 | -528 | ClearRegion | Stub |
| 89 | -534 | DisposeRegion | Stub |
| 90 | -540 | FreeVPortCopLists | Stub |
| 91 | -546 | FreeCopList | Stub |
| 92 | -552 | ClipBlit | Stub |
| 93 | -558 | XorRectRegion | Stub |
| 94 | -564 | FreeCprList | Stub |
| 95 | -570 | GetColorMap | Stub |
| 96 | -576 | FreeColorMap | Stub |
| 97 | -582 | GetRGB4 | Stub |
| 98 | -588 | ScrollVPort | Stub |
| 99 | -594 | UCopperListInit | Stub |
| 100 | -600 | FreeGBuffers | Stub |
| 101 | -606 | BltBitMapRastPort | Stub |
| 102 | -612 | OrRegionRegion | Stub |
| 103 | -618 | XorRegionRegion | Stub |
| 104 | -624 | AndRegionRegion | Stub |
| 105 | -630 | SetRGB4CM | Stub |
| 106 | -636 | BltMaskBitMapRastPort | Stub |
| 107 | -642 | *reserved* | No-op |
| 108 | -648 | *reserved* | No-op |
| 109 | -654 | AttemptLockLayerRom | Stub |
| 110 | -660 | GfxNew | Stub |
| 111 | -666 | GfxFree | Stub |
| 112 | -672 | GfxAssociate | Stub |
| 113 | -678 | BitMapScale | Stub |
| 114 | -684 | ScalerDiv | Stub |
| 115 | -690 | TextExtent | Stub |
| 116 | -696 | TextFit | Implemented |
| 117 | -702 | GfxLookUp | Stub |
| 118 | -708 | VideoControl | Stub |
| 119 | -714 | OpenMonitor | Stub |
| 120 | -720 | CloseMonitor | Stub |
| 121 | -726 | FindDisplayInfo | Stub |
| 122 | -732 | NextDisplayInfo | Stub |
| 123 | -738 | *reserved* | No-op |
| 124 | -744 | *reserved* | No-op |
| 125 | -750 | *reserved* | No-op |
| 126 | -756 | GetDisplayInfoData | Stub |
| 127 | -762 | FontExtent | Stub |
| 128 | -768 | ReadPixelLine8 | Implemented |
| 129 | -774 | WritePixelLine8 | Implemented |
| 130 | -780 | ReadPixelArray8 | Implemented |
| 131 | -786 | WritePixelArray8 | Implemented |
| 132 | -792 | GetVPModeID | Implemented |
| 133 | -798 | ModeNotAvailable | Stub |
| 134 | -804 | WeighTAMatch | Stub |
| 135 | -810 | EraseRect | Implemented |
| 136 | -816 | ExtendFont | Stub |
| 137 | -822 | StripFont | Stub |
| 138 | -828 | CalcIVG | Stub |
| 139 | -834 | AttachPalExtra | Stub |
| 140 | -840 | ObtainBestPenA | Stub |
| 141 | -846 | *reserved* | No-op |
| 142 | -852 | SetRGB32 | Stub |
| 143 | -858 | GetAPen | Implemented |
| 144 | -864 | GetBPen | Implemented |
| 145 | -870 | GetDrMd | Implemented |
| 146 | -876 | GetOutlinePen | Implemented |
| 147 | -882 | LoadRGB32 | Stub |
| 148 | -888 | SetChipRev | Stub |
| 149 | -894 | SetABPenDrMd | Implemented |
| 150 | -900 | GetRGB32 | Stub |
| 151 | -906 | *reserved* | No-op |
| 152 | -912 | *reserved* | No-op |
| 153 | -918 | AllocBitMap | Stub |
| 154 | -924 | FreeBitMap | Stub |
| 155 | -930 | GetExtSpriteA | Stub |
| 156 | -936 | CoerceMode | Stub |
| 157 | -942 | ChangeVPBitMap | Stub |
| 158 | -948 | ReleasePen | Stub |
| 159 | -954 | ObtainPen | Stub |
| 160 | -960 | GetBitMapAttr | Implemented |
| 161 | -966 | AllocDBufInfo | Stub |
| 162 | -972 | FreeDBufInfo | Stub |
| 163 | -978 | SetOutlinePen | Implemented |
| 164 | -984 | SetWriteMask | Implemented |
| 165 | -990 | SetMaxPen | Implemented |
| 166 | -996 | SetRGB32CM | Stub |
| 167 | -1002 | ScrollRasterBF | Stub |
| 168 | -1008 | FindColor | Stub |
| 169 | -1014 | *reserved* | No-op |
| 170 | -1020 | AllocSpriteDataA | Stub |
| 171 | -1026 | ChangeExtSpriteA | Stub |
| 172 | -1032 | FreeSpriteData | Stub |
| 173 | -1038 | SetRPAttrsA | Stub |
| 174 | -1044 | GetRPAttrsA | Stub |
| 175 | -1050 | BestModeIDA | Stub |
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
