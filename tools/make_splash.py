#!/usr/bin/env python3
"""make_splash.py — Convert the boot splash artwork to the kernel blob format.

Usage:
    python3 tools/make_splash.py <input image> <output blob>

Output format (little-endian, self-describing — see kernel/display/splash.c):

    [0:4]   "SPL0" magic
    [4:8]   u32 width
    [8:12]  u32 height
    [12:16] u32 border colour 0xRRGGBB (top-left pixel — used to fill the
            screen around the centred image)
    [16:]   RGB24 pixel data, row-major, top-down

The build links the blob into the kernel with `ld -r -b binary`, so the only
requirement on the host is python3 + Pillow.
"""

import struct
import sys

from PIL import Image


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 1

    src, dst = sys.argv[1], sys.argv[2]
    im = Image.open(src).convert("RGB")
    w, h = im.size
    r, g, b = im.getpixel((0, 0))
    bg = (r << 16) | (g << 8) | b

    with open(dst, "wb") as f:
        f.write(b"SPL0")
        f.write(struct.pack("<III", w, h, bg))
        f.write(im.tobytes())

    print(f"splash: {src} -> {dst} ({w}x{h}, bg=#{bg:06X}, {16 + w * h * 3} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
