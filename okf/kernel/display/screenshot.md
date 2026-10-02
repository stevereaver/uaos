---
type: Kernel Component
title: Screenshot command + in-kernel JPEG encoder
description: C:screenshot captures the composited screen to a baseline JPEG on RAM:, named by capture date/time; jpeg_enc.c is a freestanding baseline JPEG encoder.
resource: /kernel/display/jpeg_enc.c
tags: [display, screenshot, jpeg, framebuffer]
timestamp: 2026-10-02T00:00:00Z
---

# Screenshot & JPEG Encoder (UAOS-218)

## `C:screenshot` — `kernel/shell/cmd_screenshot.c`

Captures the whole framebuffer to a baseline JPEG file.

```
screenshot              -> RAM:<YYYYMMDDHHMMSS>.jpg
screenshot FILE=path    -> explicit destination (any VFS volume)
screenshot Q=n          -> JPEG quality 1..100 (default 85)
```

- Default filename is a 14-digit serial built from the capture
  date/time: NTP-synced local time (`ntp_get_epoch` +
  `tz_offset_min`) when `ntpd` has run, else CMOS RTC (UTC), the same
  source `C:date` uses. A second capture inside the same second gets a
  `_N` suffix; with no valid clock at all the name falls back to a
  monotonic `shot<N>.jpg`.
- Pixels come from `FB_GetPixel` — the composited back buffer once it
  is coherent, VRAM before that. The pointer sprite (drawn straight to
  VRAM at flip time) is not captured.
- Failed/short writes delete the partial file.

## `jpeg_enc.{c,h}` — baseline JPEG encoder

Freestanding, no libc, no image-size buffer: pixels arrive through a
`JpegPixelFn` callback (edge-clamped, 0x00RRGGBB) and the stream leaves
through a `JpegWriteFn` callback (1 KB internal flush buffer).

- JFIF APP0, DQT (luma+chroma, zigzag order), SOF0 (3 components,
  Y sampled 2x2 → 16x16 MCUs), DHT (all four Annex-K tables), SOS,
  EOI; 0xFF byte stuffing in entropy data; all-ones final pad.
- 4:2:0 chroma via 2x2 RGB averaging; integer JFIF RGB→YCbCr.
- FDCT is a direct separable transform through a precomputed Q14 8x8
  matrix (`F = M·f·M^T`, ~1024 mults/block) — deliberately simple over
  AAN/Loeffler. Quality scaling uses the libjpeg formula
  (`q<50 ? 5000/q : 200-2q`, clamp 1..255).

## Gotcha — task stack alignment (the bug this exposed)

The first `screenshot` run #GP'd inside `Jpeg_Encode` on a
`movdqa` to a stack array: `g_task_stacks` is only `aligned(8)`, so
`Task_CreateNative`'s entry RSP could land ≡0 mod 16 instead of the
SysV-required ≡8. `Task_CreateNative` now explicitly 16-aligns the
frame top before planting the `Task_Exit` return slot
(`kernel/exec/task.c`), making entry alignment independent of BSS
layout — the UAOS-182 bug class, permanently fixed.

## Testing

Host harness: compile `jpeg_enc.c` standalone against a test main
(pixel callback + fwrite sink), decode the output with PIL/djpeg.
In-guest: boot QEMU (`scripts/run_with_disk.sh`, telnetd auto-starts),
`screenshot` in a telnet shell, extract with
`gnu:usr/bin/base64 RAM:<file>.jpg` and decode host-side.
