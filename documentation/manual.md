# Ultimate Amiga OS — Technical Reference Manual

**Version 0.1.0-dev** | Generated: 2026

---

## Table of Contents

1. [Introduction](#introduction)
2. [System Architecture](#system-architecture)
3. [Kernel Subsystems](#kernel-subsystems)
4. [ROM Module System](#rom-module-system)
5. [Graphics and Window Manager](#graphics-and-window-manager)
6. [Input Drivers](#input-drivers)
7. [Filesystem Layer](#filesystem-layer)
8. [Shell](#shell)
9. [Build System](#build-system)
10. [Running in QEMU](#running-in-qemu)
11. [Troubleshooting](#troubleshooting)
12. [Memory Map Reference](#memory-map-reference)

---

## Introduction

Ultimate Amiga OS (UAOS) is a standalone bare-metal x86_64 operating system
designed to provide a native Amiga-compatible environment on modern hardware
without requiring any host operating system. It boots directly from a hybrid
UEFI/BIOS ISO via GRUB2 Multiboot2, initialises a linear framebuffer through
UEFI GOP, and presents a graphical Workbench 3.x-style desktop with a
fully-functional window manager, PS/2 mouse and keyboard drivers, and an
interactive shell.

### Project Goals

- Provide a Workbench 3.x-style graphical environment natively on x86_64
- Implement transparent M68k emulation to run legacy Amiga Hunk binaries
- Intercept Amiga Exec/DOS/Intuition system calls via illegal-opcode traps and dispatch to native 64-bit C implementations
- Use MMU 2 MB huge pages to virtualise the Amiga custom chip register window via `#PF` exceptions

### Architectural Constraints

- **No Linux kernel** — UAOS boots directly into its own x86_64 microkernel
- **Trap-based thunking** — M68k binaries are loaded into a 4 GB sandbox; system calls intercepted via `ILLEGAL + $414D` magic-token sequences
- **MMU hardware virtualisation** — the Amiga hardware register window `0x00B00000–0x00DFFFFF` is marked NOT PRESENT, faults routed to the chip emulator
- **Amiga filesystem namespace** — `dos.library` handlers resolve `SYS:`, `S:`, `C:`, `RAM:` assigns alongside POSIX translation paths

---

## System Architecture

### Boot Sequence

1. GRUB2 loads the ELF64 kernel via the Multiboot2 protocol and fills the framebuffer tag from the UEFI GOP driver.
2. `uaos_kernel_entry.asm` — NASM bootstrap:
   - Transitions CPU from 32-bit protected mode to 64-bit long mode
   - Sets up a bootstrap identity-map page table
   - Calls `uaos_kernel_main()`
3. `uaos_kernel_main()` (C):
   - Validates Multiboot2 magic value
   - Initialises VGA text console and UART (COM1, 38400 baud)
   - Calls `FB_Init()` to parse the Multiboot2 framebuffer tag
   - Calls `IDT_Init()` to install the 256-vector IDT and remap the 8259A PIC
   - Calls `UAOS_MMU_Init()` to install the sandbox page tables
   - Calls `UAOS_ROM_RegisterAll()` to populate the ROM module registry
   - Calls `UAOS_Bridge_Init()` to allocate the 4 GB guest RAM window
   - Calls `PS2Mouse_Init()` and `PS2Kbd_Init()`
   - Renders the desktop and registers the shell window with the WM
   - Enters the `hlt`-loop event dispatcher

### M68k Thunk Dispatch

The thunk mechanism replaces selected Exec jump-table vectors in the AROS ROM
with 8-byte breakout stubs using the `MK_THUNK` macro in
`emulation/rom_patches/rom_traps.s`:

```asm
MK_THUNK MACRO
        ILLEGAL             ; $4AFC — triggers JIT breakout
        dc.w    \1          ; UAOS magic token ($414D = "AM")
        dc.w    \2          ; function index (1-based)
        rts                 ; return to guest after dispatch
        ENDM
```

When the JIT core decodes `0x4AFC` it invokes `UAOS_Bridge_IllegalOpcode()`. The C dispatcher:

1. Reads the two words immediately following the ILLEGAL opcode
2. Validates the `$414D` signature
3. Extracts arguments from the M68k register state (A1, D0, D1, …)
4. Calls the native 64-bit stub
5. Writes return values back into D0/A0
6. Advances the guest PC by 6 bytes

### MMU Sandbox

`kernel/exec/mmu_sandbox.c` constructs four-level x86_64 paging tables mapping
the entire 4 GB guest address space using 2 MB huge pages.

| Address Range | Size | Description |
|---|---|---|
| `0x00000000–0x001FFFFF` | 2 MB | Chip RAM |
| `0x00200000–0x009FFFFF` | 8 MB | Fast RAM (lower) |
| `0x00B00000–0x00DFFFFF` | 3 MB | **Custom chip registers (NOT PRESENT)** |
| `0x00F80000–0x00FFFFFF` | 512 KB | Kickstart ROM mirror |
| `0x01000000–0x7FFFFFFF` | 2 GB | Extended Fast RAM |
| `0x80000000–0xFFFFFFFF` | 2 GB | x86_64 kernel space |

Pages in the hardware register window have `PAGE_PRESENT` cleared. Any access
faults to this region are caught by the `#PF` handler in
`kernel/exec/page_fault_handler.c` and forwarded to the chip emulator.

---

## Kernel Subsystems

### ROM Module Registry (`rom_modules.c`)

`UAOS_ROM_RegisterAll()` is called once at boot. It populates a static array of
`UaosRomModule` descriptors, each holding:

- Library name string (e.g. `"exec.library"`)
- Library version number
- 32-bit Amiga base address
- Array of native function pointers (indexed 1-based to match `rom_traps.s` function indices)

### Thunk Handler (`thunk_handler.c`)

The `UAOS_AMIGA_TO_HOST()` macro translates any 32-bit Amiga guest address to
the corresponding host linear address:

```c
#define UAOS_AMIGA_TO_HOST(amiga_addr) \
    ((void *)((uintptr_t)(uaos_ram_base) + (uint32_t)(amiga_addr)))
```

### Page Fault Handler (`page_fault_handler.c`)

The ISR entry stub `uaos_page_fault_isr` (inline GAS assembly) saves all 15
GPRs onto the kernel stack, computes pointers to the `InterruptFrame` and
`SavedRegs` blocks, and calls `UAOS_PageFaultHandler()`. On return, GPRs are
restored and `IRETQ` resumes execution.

### IDT and PIC (`idt.c`)

- 256-vector Interrupt Descriptor Table installed via `LIDT`
- 8259A PIC remapped: IRQ0–7 → vectors 32–39, IRQ8–15 → vectors 40–47
- IRQ1 (vector 33) — PS/2 keyboard
- IRQ8 (vector 40) — RTC (CMOS real-time clock)
- IRQ12 (vector 44) — PS/2 mouse

### RTC Driver (`rtc.c`)

CMOS real-time clock driver providing time snapshots and UIE interrupt support:

- **I/O ports**: 0x70 (index), 0x71 (data)
- **Functions**: `RTC_ReadTime()` reads current time into `RtcTime` structure
- **IRQ handler**: Updates cached time on each UIE interrupt
- **Connected to**: `timer.device` for timing functions

### VirtIO Block Device Driver (`virtio_blk.c`)

VirtIO block device driver for QEMU external disk support:

- **PCI scanning**: Searches for VirtIO block device (vendor 0x1AF4, device 0x1001)
- **BAR0**: I/O space for VirtIO device configuration
- **Capacity reporting**: Reads device capacity from configuration space
- **Registered as**: "virtio0" with block device layer
- **Status**: PCI scanning and capacity reporting implemented; virtqueue I/O marked as TODO

---

## ROM Module System

UAOS implements a native ROM module system that provides AmigaOS-compatible library and device implementations. These modules are registered at boot and can be called by M68k emulated code via ILLEGAL opcode dispatch.

### ROM Module Registry (`rom_modules.c`)

`UAOS_ROM_RegisterAll()` is called once at boot. It populates a static array of `UaosRomModule` descriptors, each holding:

- Library name string (e.g. `"exec.library"`)
- Library version number
- 32-bit Amiga base address
- Array of native function pointers (indexed 1-based to match LVO offsets)

### Registered ROM Modules

#### Libraries

| Library | Version | Base Address | Functions |
|---------|---------|-------------|-----------|
| `exec.library` | v45 | 0x000000D0 | Process management, memory allocation |
| `utility.library` | v37 | 0x000000E0 | String functions, memory utilities |
| `console.device` | v40 | 0x000000F0 | Console I/O |
| `mathffp.library` | v40 | 0x00000070 | Software floating-point (uses softfloat) |
| `locale.library` | v38 | 0x00000100 | Localization support |
| `ixemul.library` | v53 | 0x00000110 | Unix compatibility layer |
| `graphics.library` | v40 | 0x00000120 | Graphics primitives |
| `dos.library` | v40 | 0x000000D0 | File system operations |

#### Devices

| Device | Version | Base Address | Connected To |
|--------|---------|-------------|-------------|
| `timer.device` | v40 | 0x00000130 | RTC driver (CMOS) |
| `keyboard.device` | v40 | 0x00000140 | PS/2 keyboard driver |

### Library Function Implementation

Each library is implemented in `kernel/exec/` with the pattern:

```c
/* Function indices matching AmigaOS LVO offsets */
#define LIBRARY_FUNCTION_1   1
#define LIBRARY_FUNCTION_2   2
/* ... */

static void library_Function1(void)
{
    /* Implementation with M68k register access */
    /* D0, D1, A0, A1 contain arguments */
    /* Return value in D0/A0 */
}

static void *library_funcs[] = {
    library_Function1,   /* index 1 */
    library_Function2,   /* index 2 */
    /* ... */
};

void UAOS_LIBRARY_Register(void)
{
    UAOS_ROM_Register("library.name", version, base_addr,
                      (uint16_t)(sizeof(library_funcs) / sizeof(library_funcs[0])),
                      library_funcs);
}
```

### M68k Integration

Functions are called via ILLEGAL opcode dispatch in `uaos_m68k_glue.c`:

1. M68k code calls library function via LVO
2. ILLEGAL opcode triggers dispatch
3. Arguments read from M68k registers (D0, D1, A0, A1)
4. Native C function called
5. Return values written back to M68k registers
6. Execution resumes in M68k code

### Current Implementation Status

- **exec.library**: Basic stubs for OpenLibrary, CloseLibrary, AllocMem, FreeMem, FindTask
- **utility.library**: String functions (StrIcmp, StrNicmp, UcStr, LcStr) with implementation logic
- **mathffp.library**: All 18 floating-point functions with softfloat integration logic
- **dos.library**: Lock/Unlock, Examine/ExamineNext with implementation logic
- **timer.device**: Connected to RTC driver for timing functions
- **keyboard.device**: Connected to PS/2 keyboard driver for input

Functions currently log calls and have detailed implementation logic in comments. Full M68k memory access integration is pending.

---

## Graphics and Window Manager

### Framebuffer (`framebuffer.c`)

GRUB2 fills the Multiboot2 framebuffer tag (type 8) from the UEFI GOP driver.
`FB_Init()` parses this tag to populate the global `FbState g_fb` structure:

```c
typedef struct {
    uint64_t  phys_addr;   /* physical base address */
    uint32_t  width;       /* pixels wide           */
    uint32_t  height;      /* pixels tall           */
    uint32_t  pitch;       /* bytes per row         */
    uint8_t   bpp;         /* 24 or 32              */
    uint8_t   valid;
} FbState;
```

All drawing primitives clip coordinates to screen bounds before writing,
correctly handling windows that extend partially off-screen (including negative
x/y coordinates):

| Function | Description |
|---|---|
| `FB_FillRect(x, y, w, h, col)` | Filled rectangle |
| `FB_DrawRect(x, y, w, h, col)` | Outline rectangle |
| `FB_DrawHLine(x, y, len, col)` | Horizontal line |
| `FB_DrawVLine(x, y, len, col)` | Vertical line |
| `FB_PutPixel(x, y, col)` | Single pixel |
| `FB_PutStr(x, y, s, fg, bg)` | 8×16 bitmap font string |

### Desktop (`desktop.c`)

`Desktop_Draw()` renders the full Workbench-style backdrop:

- **Menu bar** (top, 20 px) — Workbench menu labels and clock area
- **Backdrop** — light grey base (`#AAAAAA`) with a dark-grey (`#555555`) stipple dot overlay; one dot per two pixels on alternating rows/columns — the classic Amiga pattern
- **Disk icons** — top-right corner, stacked vertically (RAM Disk, UAOS:)
- **Information window** — centred boot status window
- **Status bar** (bottom, 18 px) — kernel idle message and RAM size

`Desktop_RedrawRect(rx, ry, rw, rh)` repaints a sub-rectangle of the desktop
(backdrop + all overlays: icons, info window, menu bar, status bar) without a
full-screen repaint. This is called by the window manager on every drag/resize
step to erase the old window footprint efficiently.

### Window Manager (`wm.c`)

A lightweight WM supporting up to 8 simultaneous windows.

#### WmWindow structure

```c
typedef struct {
    int       x, y, w, h;
    char      title[32];
    WM_DrawFn draw;      /* repaint callback: (x, y, w, h) */
    WM_KeyFn  on_key;    /* keystroke callback: (char c)   */
    int       active;
} WmWindow;
```

#### Z-Order and Focus

Windows are stored in `g_wins[]` and indexed by a separate `g_zorder[]` array
(`[0]` = back, `[g_nwins-1]` = top). `WM_Redraw()` paints windows
back-to-front. Clicking a window calls `raise_window()` to promote it to the
top of the z-stack.

#### Drag and Resize

- **Drag** — mouse-down on the title bar records the grab offset. Each
  subsequent mouse move calls `Desktop_RedrawRect` on the old footprint, updates
  `w->x/y`, then repaints all windows front-to-back.
- **Resize** — mouse-down on the 16×16 resize grip (bottom-right corner)
  records the base size and drag origin. Mouse movement computes new dimensions
  relative to the drag start point. Minimum size: 120×80 px.
- Windows may extend off the left, right, and bottom screen edges. The title
  bar is always constrained to remain at or below the menu bar (`y >= 20`), and
  at least 32 px of horizontal width stays on-screen.

### Software Cursor (`cursor.c`)

A 16×16 Amiga-style arrow sprite cursor rendered in software:

1. Save the 16×16 background pixels beneath the cursor position
2. Draw the sprite (two-colour XOR mask)
3. On each move: restore saved background, save new background, draw sprite at new position

This ensures desktop and window content are never permanently overwritten by
cursor movement.

---

## Input Drivers

### PS/2 Mouse (`ps2mouse.c`)

- **IRQ12** (vector 44), 3-byte packet protocol
- Byte 0: flags (button state, overflow, sign bits)
- Bytes 1–2: signed X and Y deltas (two's complement with sign extension from flags)
- Cursor position clamped to `[0, screen_width-1]` × `[0, screen_height-1]`
- Calls `Cursor_Move(new_x, new_y)` on each valid packet

### PS/2 Keyboard (`ps2kbd.c`)

- **IRQ1** (vector 33), scancode set 1
- Ring buffer stores decoded ASCII characters
- `PS2Kbd_HasChar()` / `PS2Kbd_GetChar()` polled by the event loop
- Keystrokes routed to the focused window via `WM_KeyEvent(c)`

---

## Filesystem Layer

UAOS provides a layered filesystem architecture supporting multiple filesystem types through a unified VFS (Virtual Filesystem) interface.

### Block Device Layer (`blockdev.c`)

The block device layer provides a unified interface for storage devices:

```c
typedef struct BlockDev {
    const char *name;           /* Device name (e.g., "virtio0") */
    uint32_t  sector_size;     /* Sector size in bytes (usually 512) */
    uint64_t  num_sectors;     /* Total number of sectors */
    void     *private_data;    /* Driver-specific data */
    const BlockDevOps *ops;    /* Device operations */
    struct BlockDev *next;     /* Next device in list */
} BlockDev;
```

#### Block Device Operations

| Function | Description |
|----------|-------------|
| `BlockDev_Register(dev)` | Register a block device |
| `BlockDev_Unregister(dev)` | Unregister a block device |
| `BlockDev_Find(name)` | Find device by name |
| `BlockDev_Read(dev, sector, buffer, num)` | Read sectors |
| `BlockDev_Write(dev, sector, buffer, num)` | Write sectors |
| `BlockDev_GetCapacity(dev)` | Get device capacity |

#### Supported Block Devices

- **VirtIO Block Device** (`virtio_blk.c`): PCI scanning, device detection, capacity reporting
  - Registers as "virtio0"
  - Currently stub implementation (virtqueue I/O marked as TODO)

### VFS Layer (`vfs.c`)

The VFS layer provides a thin dispatch over filesystem implementations:

```c
typedef struct {
    RamFsNode *node;    /* NULL = invalid / not open */
    uint32_t   pos;     /* current read/write position */
} VfsFile;
```

#### VFS Operations

| Function | Description |
|----------|-------------|
| `VFS_Init()` | Initialize VFS and mount RAM: |
| `VFS_Open(fh, path, flags)` | Open a file |
| `VFS_Close(fh)` | Close a file handle |
| `VFS_Read(fh, buf, len)` | Read from file |
| `VFS_Write(fh, buf, len)` | Write to file |
| `VFS_Seek(fh, pos)` | Seek to position |
| `VFS_Size(fh)` | Get file size |
| `VFS_MkDir(path)` | Create directory |
| `VFS_Delete(path)` | Delete file/directory |
| `VFS_OpenDir(path)` | Open directory for reading |
| `VFS_ResolveDir(path)` | Resolve path to directory node |
| `VFS_GetRoot(vol_name)` | Get volume root node |

### RAM Filesystem (`ramfs.c`)

In-memory filesystem with BSS-backed storage:

- **Node tree**: 256 nodes maximum
- **Data pool**: 64KB for file contents
- **Auto-mounted at boot**: RAM: with T, ENV, CLIPS, S directories
- **Include style**: `#include "dos/vfs.h"` (kernel root relative)

### Filesystem Drivers

#### FAT32 (`fat32.c`)

FAT32 filesystem driver for block devices:

```c
typedef struct {
    BlockDev *bdev;           /* Block device */
    Fat32BPB  bpb;            /* Boot sector */
    uint32_t  fat_start;      /* FAT start sector */
    uint32_t  data_start;     /* Data start sector */
    uint32_t  root_cluster;   /* Root directory cluster */
    uint32_t  bytes_per_sec;  /* Bytes per sector */
    uint32_t  sec_per_clus;   /* Sectors per cluster */
    uint32_t  cluster_size;   /* Cluster size in bytes */
    uint8_t  *fat_cache;      /* FAT cache */
    uint32_t  fat_cache_sec;  /* Cached FAT sector */
} Fat32FS;
```

**Operations**: Mount, Unmount, Open, Close, Read, Write, Seek, Size, ReadDir

**Status**: Mount function implemented with boot sector parsing. File operations are stubs with implementation logic.

#### PFS3 (`pfs3.c`)

Professional File System 3 (Amiga-specific) driver:

```c
typedef struct {
    BlockDev *bdev;           /* Block device */
    Pfs3RootBlock root;       /* Root block */
    uint32_t  block_size;     /* Block size */
    uint32_t  total_blocks;   /* Total blocks */
    uint32_t  root_block;     /* Root block number */
    uint32_t  bitmap_start;   /* Bitmap start block */
    uint32_t  bitmap_blocks;  /* Number of bitmap blocks */
} Pfs3FS;
```

**Operations**: Mount, Unmount, Open, Close, Read, Write, Seek, Size, ReadDir

**Status**: Mount function implemented with signature validation. File operations are stubs.

#### EXT4 (`ext4.c`)

Linux EXT4 filesystem driver:

```c
typedef struct {
    BlockDev *bdev;           /* Block device */
    Ext4Superblock sb;         /* Superblock */
    uint32_t  block_size;     /* Block size */
    uint32_t  inode_size;     /* Inode size */
    uint32_t  blocks_per_group;/* Blocks per group */
    uint32_t  inodes_per_group;/* Inodes per group */
    uint32_t  inode_table_start;/* Inode table start */
} Ext4FS;
```

**Operations**: Mount, Unmount, Open, Close, Read, Write, Seek, Size, ReadDir

**Status**: Mount function implemented with superblock parsing. File operations are stubs.

### Shell Commands

The shell integrates with the VFS layer for filesystem operations:

| Command | Description |
|---------|-------------|
| `dir` | List files in current directory |
| `cd` | Change current directory |
| `makedir` | Create a directory |
| `delete` | Delete a file or directory |
| `type` | Display file contents |
| `copy` | Copy a file |

### Current Implementation Status

- **Block device layer**: Complete with VirtIO registration
- **VFS layer**: Complete with RAM filesystem
- **FAT32**: Mount implemented, file operations stubs
- **PFS3**: Mount implemented, file operations stubs
- **EXT4**: Mount implemented, file operations stubs

All filesystem drivers use static allocation (no malloc) for freestanding environment. File operations have detailed implementation logic in comments pending M68k memory access integration.

---

## Shell

The shell window (`shell_win.c`) registers with the WM via `WM_AddWindow()` and
implements a scrollable terminal:

- History buffer: up to 128 lines × 128 characters
- Input line with backspace support
- Dynamic layout — adapts to window resize

### Built-in Commands

| Command | Description |
|---------|-------------|
| `help` | List available commands |
| `version` | Show kernel version and CPU architecture |
| `mem` | Display total RAM from Multiboot2 tag |
| `clear` | Clear the shell history buffer |
| `reboot` | Trigger a system reboot via port `0x64` |
| `libs` | List loaded kernel libraries with versions |
| `dir` | List files in current directory (VFS) |
| `cd` | Change current directory (VFS) |
| `makedir` | Create a directory (VFS) |
| `delete` | Delete a file or directory (VFS) |
| `type` | Display file contents (VFS) |
| `copy` | Copy a file (VFS) |

---

## Build System

### Dependencies

```bash
sudo apt install \
    nasm \
    gcc \
    binutils \
    grub-pc-bin \
    grub-efi-amd64-bin \
    grub-common \
    xorriso \
    ovmf \
    qemu-system-x86
```

### Build Pipeline

```bash
# Full clean build
bash scripts/build_iso.sh --clean

# Incremental build
bash scripts/build_iso.sh
```

Output: `build/Ultimate_Amiga_OS.iso`

#### What the script does

| Step | Action |
|---|---|
| 1 | Creates `build/` staging directories |
| 2 | Assembles `uaos_kernel_entry.asm` + `idt_stubs.asm` with NASM |
| 3 | Compiles all C sources: `-ffreestanding -m64 -O2 -std=c11 -fno-stack-protector` |
| 4 | Links into `uaos-kernel.elf` (ELF64) via the custom linker script |
| 5 | Packages the `sys-root` Amiga filesystem skeleton |
| 6 | Injects `grub.cfg` |
| 7 | Builds `bootx64.efi` with `grub-mkstandalone` |
| 8 | Produces hybrid BIOS+EFI ISO with `grub-mkrescue` |

### GCC Flags

```
-ffreestanding -fno-stack-protector -fno-pie -fno-PIE
-mno-red-zone -nostdlib -m64 -O2 -std=c11
-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
-Wall -Wextra
```

---

## Running in QEMU

### One-time OVMF Setup

```bash
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/ovmf_vars.fd
```

### Launch Command

```bash
qemu-system-x86_64 \
  -machine q35,usb=off \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=/tmp/ovmf_vars.fd \
  -cdrom build/Ultimate_Amiga_OS.iso \
  -m 512M \
  -vga virtio \
  -no-reboot
```

| Flag | Purpose |
|---|---|
| `-machine q35,usb=off` | Q35 chipset; `usb=off` prevents USB tablet conflicting with PS/2 mouse |
| `OVMF_CODE_4M.fd` | UEFI firmware (read-only) |
| `ovmf_vars.fd` | UEFI variable store (writable copy) |
| `-vga virtio` | Best framebuffer performance |
| `-no-reboot` | Keeps QEMU open after kernel reboot call |

### Serial Debug Output

```bash
# To terminal
qemu-system-x86_64 ... -serial stdio

# To file
qemu-system-x86_64 ... -serial file:/tmp/uaos_serial.log
```

---

## Troubleshooting

### GRUB: "Invalid arch-dependent ELF magic"

`uaos-kernel.elf` is not a valid ELF64 binary. Ensure the full NASM + GCC + LD
pipeline completed without errors. Run `file build/uaos-kernel.elf` — it must
report `ELF 64-bit LSB executable, x86-64`.

### GRUB: "You need to load the kernel first"

GRUB could not find the `multiboot2` header within the first 32 KB of the
binary. Verify the linker script places `.multiboot2` first and that the NASM
source emits the section correctly.

### Kernel halts immediately after boot

Enable serial output to see debug messages:

```bash
qemu-system-x86_64 ... -serial stdio 2>&1 | tee boot.log
```

### Mouse jitters or snaps back to centre

Ensure QEMU is launched with `-machine q35,usb=off`. The USB tablet device
provides absolute coordinates that conflict with the PS/2 relative mouse driver.

### Mouse cursor not visible

The cursor is rendered in software. If the framebuffer is not initialised (no
`[BOOT] Framebuffer: ...` line in serial log), GRUB did not provide a
framebuffer tag — check `grub.cfg` for `set gfxmode` and `insmod all_video`.

---

## Memory Map Reference

| Address Range | Size | Description |
|---|---|---|
| `0x00000000–0x001FFFFF` | 2 MB | Chip RAM |
| `0x00200000–0x009FFFFF` | 8 MB | Fast RAM (lower) |
| `0x00A00000–0x00AFFFFF` | 1 MB | Slow RAM / ranger |
| `0x00B00000–0x00DFFFFF` | 3 MB | **Custom chip registers (NOT PRESENT — #PF trap)** |
| `0x00E00000–0x00EFFFFF` | 1 MB | Extended ROM / card space |
| `0x00F80000–0x00FFFFFF` | 512 KB | Kickstart ROM mirror |
| `0x01000000–0x7FFFFFFF` | 2 GB | Extended Fast RAM |
| `0x80000000–0xFFFFFFFF` | 2 GB | x86_64 kernel space |

---

*Copyright © 2026 UAOS Development Team*
