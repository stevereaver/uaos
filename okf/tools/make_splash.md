---
type: Tool
title: make_splash.py
description: Build-time converter that turns splash.jpg into a self-describing RGB24 blob for embedding into the kernel as a binary object.
resource: /tools/make_splash.py
tags: [tool, build, splash, display]
status: stable
generated: { by: agent:devin, at: 2026-09-29T00:00:00Z }
---

# make_splash.py

Converts the boot splash artwork (repo-root `splash.jpg`) into a raw
self-describing RGB24 blob consumed by `kernel/display/splash.c`
(UAOS-151). Requires Pillow. Invoked by `scripts/build_iso.sh`; the
output (`build/obj/splash.rgb`) is wrapped with `ld -r -b binary` into
`splash_img.o` and linked into `uaos-kernel.elf`, so the kernel needs no
image decoder.

## Blob layout (little-endian)

| Offset | Field |
|--------|-------|
| 0      | `"SPL0"` magic |
| 4      | u32 width |
| 8      | u32 height |
| 12     | u32 border colour `0xRRGGBB` — sampled edge colour; `Splash_Show` fills the screen with it so letterboxing blends seamlessly |
| 16     | RGB24 pixel data, row-major, top-down |

The image is decoded to RGB first, so any Pillow-readable format works —
the filename is not restricted to JPEG. The border colour is taken from
the image's corner/edge pixels (currently `#000000`).

A binary blob is used instead of the `embed_binary.sh` C-array pattern
because the converted image is ~1.7 MB — a generated C source would be
enormous and slow to compile.

## Usage

```bash
tools/make_splash.py splash.jpg build/obj/splash.rgb
```

Prints `splash: <in> -> <out> (<w>x<h>, bg=#RRGGBB, <n> bytes)`.
