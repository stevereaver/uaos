#!/usr/bin/env python3
"""hdf2vdi.py — convert a raw hardfile (.hdf, e.g. a WinUAE disk image)
into a VirtualBox VDI image.

An .hdf is just raw sectors with no container, so this tool wraps the
data in a VDI header:

  * dynamic VDI (default): sparse — all-zero 1 MiB chunks are marked
    free in the block allocation table instead of being written out.
  * --fixed: a fixed VDI — every block pre-allocated, raw bytes
    stored contiguously after the header and block table.

The header layout follows what VirtualBox itself writes (VDI 1.1+,
including the LCHS geometry extension at the end of VDIHEADER1PLUS):
  cbHeader = 400, LegacyGeometry marked not-calculated (0,0,0,512),
  LCHSGeometry = real CHS + cbSector 512, non-null create/modify
  UUIDs, offBlocks/offData aligned to 1 MiB.

Usage:
    hdf2vdi.py input.hdf output.vdi [--fixed] [--block-size N]

The resulting .vdi can be attached in VirtualBox (Storage -> SATA/IDE
controller -> hard disk). The image is byte-identical at the sector
level, so an RDB/FFS volume mounts exactly as it does under WinUAE/QEMU.
"""

import os
import struct
import sys
import uuid

VDI_SIG_TEXT = b"<<< Oracle VirtualBox Disk Image >>>\n"
VDI_SIGNATURE = 0xBEDA107F
VDI_VERSION = 0x00010001           # VDI 1.1
VDI_PREHEADER_SIZE = 72
VDI_HEADER1_SIZE = 384
VDI_HEADER1PLUS_SIZE = 400         # VDIHEADER1 + LCHSGeometry
VDI_DATA_ALIGN = 0x100000          # 1 MiB, same as VDI_DATA_ALIGN
VDI_TYPE_DYNAMIC = 1
VDI_TYPE_FIXED = 2
VDI_BLOCK_SIZE = 0x100000          # 1 MiB allocation blocks
VDI_SECTOR_SIZE = 512
VDI_BAT_FREE = 0xFFFFFFFF          # block not allocated


def usage(status=1):
    print(__doc__.strip().split("\n\n")[0])
    print("\nUsage: hdf2vdi.py input.hdf output.vdi [--fixed] [--block-size N]")
    sys.exit(status)


def align(n, a):
    return (n + a - 1) // a * a


def lchs(disk_size):
    """Legacy CHS geometry the way VirtualBox computes it."""
    heads, spt = 16, 63
    cyls = min(disk_size // (heads * spt * VDI_SECTOR_SIZE), 16383)
    return cyls, heads, spt


def build_header(disk_size, img_type, off_blocks, off_data,
                 block_size, n_blocks, n_allocated, comment):
    """Build the VDIPREHEADER + VDIHEADER1PLUS block (all fields LE)."""
    h = bytearray(VDI_PREHEADER_SIZE + VDI_HEADER1PLUS_SIZE)
    h[0:len(VDI_SIG_TEXT)] = VDI_SIG_TEXT
    struct.pack_into("<I", h, 0x40, VDI_SIGNATURE)
    struct.pack_into("<I", h, 0x44, VDI_VERSION)
    struct.pack_into("<I", h, 0x48, VDI_HEADER1PLUS_SIZE)     # cbHeader
    struct.pack_into("<I", h, 0x4C, img_type)                # u32Type
    struct.pack_into("<I", h, 0x50, 0)                       # fFlags
    desc = comment.encode("ascii", "replace")[:255]
    h[0x54:0x54 + len(desc)] = desc                          # szComment
    struct.pack_into("<I", h, 0x154, off_blocks)
    struct.pack_into("<I", h, 0x158, off_data)
    # LegacyGeometry: not calculated (0,0,0) but cbSector must be 512.
    struct.pack_into("<IIII", h, 0x15C, 0, 0, 0, VDI_SECTOR_SIZE)
    struct.pack_into("<I", h, 0x16C, 0)                      # u32Dummy
    struct.pack_into("<Q", h, 0x170, disk_size)              # cbDisk
    struct.pack_into("<I", h, 0x178, block_size)             # cbBlock
    struct.pack_into("<I", h, 0x17C, 0)                      # cbBlockExtra
    struct.pack_into("<I", h, 0x180, n_blocks)               # cBlocks
    struct.pack_into("<I", h, 0x184, n_allocated)            # cBlocksAllocated
    u = uuid.uuid4().bytes_le
    h[0x188:0x188 + 16] = u                                  # uuidCreate
    h[0x198:0x198 + 16] = u                                  # uuidModify (must be non-null)
    # uuidLinkage / uuidParentModify stay zeroed
    cyls, heads, spt = lchs(disk_size)
    struct.pack_into("<IIII", h, 0x1C8, cyls, heads, spt,
                     VDI_SECTOR_SIZE)                        # LCHSGeometry
    return bytes(h)


def write_bat(out, off_blocks, bat):
    out.seek(off_blocks)
    bat_buf = b"".join(struct.pack("<I", e) for e in bat)
    out.write(bat_buf)
    out.write(b"\0" * (align(len(bat_buf), VDI_SECTOR_SIZE) - len(bat_buf)))


def convert_dynamic(src_path, dst_path, disk_size, block_size):
    n_blocks = (disk_size + block_size - 1) // block_size
    off_blocks = align(VDI_PREHEADER_SIZE + VDI_HEADER1PLUS_SIZE,
                       VDI_DATA_ALIGN)
    off_data = align(off_blocks + n_blocks * 4, VDI_DATA_ALIGN)

    bat = [VDI_BAT_FREE] * n_blocks
    stored = 0
    chunks = {}                                   # bat index -> data

    with open(src_path, "rb") as f:
        for i in range(n_blocks):
            data = f.read(block_size)
            if not data:
                break
            if len(data) < block_size:
                data += b"\0" * (block_size - len(data))
            if any(data):
                bat[i] = stored
                chunks[i] = data
                stored += 1

    header = build_header(disk_size, VDI_TYPE_DYNAMIC, off_blocks,
                          off_data, block_size, n_blocks, stored,
                          "Converted from hdf")

    with open(dst_path, "wb") as out:
        out.write(header)
        write_bat(out, off_blocks, bat)
        out.seek(off_data)
        for i in range(n_blocks):
            if i in chunks:
                out.write(chunks[i])
    return n_blocks, stored


def convert_fixed(src_path, dst_path, disk_size, block_size):
    n_blocks = (disk_size + block_size - 1) // block_size
    off_blocks = align(VDI_PREHEADER_SIZE + VDI_HEADER1PLUS_SIZE,
                       VDI_DATA_ALIGN)
    off_data = align(off_blocks + n_blocks * 4, VDI_DATA_ALIGN)

    header = build_header(disk_size, VDI_TYPE_FIXED, off_blocks,
                          off_data, block_size, n_blocks, n_blocks,
                          "Converted from hdf")

    with open(dst_path, "wb") as out, open(src_path, "rb") as f:
        out.write(header)
        write_bat(out, off_blocks, list(range(n_blocks)))
        out.seek(off_data)
        while True:
            buf = f.read(1 << 20)
            if not buf:
                break
            out.write(buf)
    return n_blocks, n_blocks


def main(argv):
    if len(argv) < 3:
        usage()
    src, dst = argv[1], argv[2]
    fixed = "--fixed" in argv[3:]
    block_size = VDI_BLOCK_SIZE
    if "--block-size" in argv[3:]:
        i = argv.index("--block-size")
        block_size = int(argv[i + 1], 0)
        if (block_size < VDI_BLOCK_SIZE // 2
                or block_size > VDI_BLOCK_SIZE * 8
                or block_size & (block_size - 1)):
            print("block size must be a power of two between 512 KiB and 8 MiB")
            return 1

    if not os.path.isfile(src):
        print(f"no such file: {src}")
        return 1
    disk_size = os.path.getsize(src)
    if disk_size == 0 or disk_size % VDI_SECTOR_SIZE:
        print(f"{src}: size {disk_size} is not a positive multiple of "
              f"{VDI_SECTOR_SIZE}")
        return 1

    n_blocks, stored = (convert_fixed(src, dst, disk_size, block_size)
                        if fixed else
                        convert_dynamic(src, dst, disk_size, block_size))
    kind = "fixed" if fixed else "dynamic"
    print(f"{dst}: {kind} VDI, {disk_size} bytes "
          f"({disk_size // 1048576} MiB), "
          f"{stored}/{n_blocks} x {block_size // 1024} KiB blocks stored")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
