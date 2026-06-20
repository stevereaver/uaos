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
9. [TCP/IP Networking](#tcpip-networking)
10. [Build System](#build-system)
11. [Running in QEMU](#running-in-qemu)
12. [Troubleshooting](#troubleshooting)
13. [Memory Map Reference](#memory-map-reference)

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
   - Calls `APIC_Init()` to configure the local APIC for ExtINT delivery
   - Calls `FB_Init()` to parse the Multiboot2 framebuffer tag
   - Calls `IDT_Init()` to install the 256-vector IDT and remap the 8259A PIC
   - Calls `UAOS_MMU_Init()` to install the sandbox page tables
   - Calls `RTC_Init()` to start the CMOS real-time clock
   - Calls `VFS_Init()` to mount `RAM:` and create the transient directories
   - Calls `BlockDev_Init()` and scans VirtIO/IDE block devices
   - Calls `UAOS_ROM_RegisterAll()` to populate the ROM module registry
   - Calls `UAOS_Bridge_Init()` to allocate the 4 GB guest RAM window
   - Calls `Task_Init()` to set up the Ring-3 task scheduler and TSS
   - Calls `PS2Mouse_Init()` and `PS2Kbd_Init()`
   - Calls `net_stack_init()` to bring up the TCP/IP stack (optional)
   - Renders the desktop and registers the shell window with the WM
   - Runs `S:Startup-Sequence`
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
| `0x00A00000–0x00AFFFFF` | 1 MB | Slow RAM / ranger |
| `0x00B00000–0x00DFFFFF` | 3 MB | **Custom chip registers (NOT PRESENT)** |
| `0x00E00000–0x00EFFFFF` | 1 MB | Extended ROM / card space |
| `0x00F80000–0x00FFFFFF` | 512 KB | Kickstart ROM mirror |
| `0x01000000–0x7FFFFFFF` | 2 GB | Extended Fast RAM |
| `0x80000000–0xFFFFFFFF` | 2 GB | x86_64 kernel space |

Pages in the hardware register window have `PAGE_PRESENT` cleared. Any access
faults to this region are caught by the `#PF` handler in
`kernel/exec/page_fault_handler.c` and forwarded to the chip emulator.

### Ring-3 Userspace and System Calls (`INT 0x80`)

UAOS implements a Ring-3 (user-mode) task execution model for native 64-bit ELF binaries. These programs are compiled with `-ffreestanding -nostdlib -fPIE -pie` and linked against [uaos_start.o](file:///home/reaver/uaos/system/libuaos/uaos_start.c) to produce position-independent binaries wrapped in a 32-byte `UAOS` header (type `0x0003` / `UAOS_TYPE_X64`).

1. **GDT & TSS Setup**: The ELF64 loader in [task.c](file:///home/reaver/uaos/kernel/exec/task.c) sets up the task stack, GDT user code/data segments (`SEG_USER_CS`, `SEG_USER_DS`), and registers the task with the Task State Segment (TSS). Transition to Ring 3 is performed via an `iretq` sequence targeting the binary's entry point.
2. **INT 0x80 System Calls**: Userspace utilities interact with the kernel using software interrupts via the `INT 0x80` vector. Arguments are passed via GPR registers:
   - **RAX**: System call number (e.g. `0x02` for write, `0x0C` for getcwd)
   - **RDI, RSI, RDX**: Arguments 1, 2, 3
   - **RAX**: Return value (negative values represent errors)
   The dispatcher in [syscall_dispatch.c](file:///home/reaver/uaos/kernel/exec/syscall_dispatch.c) implements page allocation (`sys_alloc`), process management (`sys_spawn`, `sys_wait`, `sys_exit`), VFS file/directory interfaces (`sys_getcwd`, `sys_opendir`, `sys_readdir`, `sys_closedir`, `sys_stat`), and GUI windowing syscalls (`create_window`, `destroy_window`, `draw_text`, `draw_rect`, `present`, `get_event`, `set_scroll_info`, `set_scroll`).
3. **Current Userspace Programs**: `hello`, `pwd`, `file`, `strings`, `find`, and `Guide` are built from `system/userspace/` and staged into `C:` (or `Tools:` for `Guide`).
4. **Execution State & Redirection**:
   - **Per-Task CWD**: Each task struct tracks its own current working directory in `task_cwd` which VFS resolves relative paths against.
   - **Output Buffering**: Stdout writes are buffered per-task in `task_out` and flushed line-by-line via `native_print_fn` to the active shell window.
   - **Synchronous Exit**: When a child task exits via `sys_exit`, the parent task is signaled with `SIGF_CHILD`, allowing the command-line interface to wait for command completion.

---

## Kernel Subsystems

### ROM Module Registry (`rom_modules.c`)

`UAOS_ROM_RegisterAll()` is called once at boot. It populates a static array of
`UaosRomModule` descriptors, each holding:

- Library name string (e.g. `"exec.library"`)
- Library version number
- 32-bit Amiga base address
- Array of native function pointers (indexed 1-based to match `rom_traps.s` function indices)

### Native Command System (`kernel/shell/`)

The shell dispatches C: binaries through a static registry in `native_cmd.c`.
`k_native_cmds[]` maps a lowercase command name to a `NativeCmdFn` handler and an
optional AmigaDOS template string. Key components:

- **`native_cmd.c/h`** — registry, case-insensitive lookup, template parsing
- **`cmd_template.c/h`** — AmigaDOS-style argument parser (`/A`, `/K`, `/S`, `/N`, `/M`, `/F`)
- **`resident_cmd.c/h`** — in-memory resident command cache (256 KB, up to 16 slots)
- **`cmd_*.c`** — individual command implementations (65+ commands)

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

### Network Drivers

UAOS includes two network hardware drivers, both accessed through the
`NetDevice` abstraction layer. The correct driver is selected automatically
at boot via `netdev_probe()`.

#### VirtIO-Net (`virtio_net.c`)

- **PCI**: vendor `0x1AF4`, device `0x1000` (legacy VirtIO 0.9) or `0x1041` (modern)
- **Interface**: I/O-port or MMIO BAR0; split virtqueue ring protocol
- **Queue size**: 256 descriptors each for RX and TX (matches QEMU default)
- **MSI-X**: present in device but kept disabled; uses 8259A PIC IRQ
- **Used by**: QEMU (`-device virtio-net-pci,disable-modern=on`)

#### Intel 82540EM e1000 (`e1000.c`)

- **PCI**: vendor `0x8086`, device `0x100E` (and related 8254x family)
- **Interface**: 128 KB MMIO BAR0; legacy 16-byte TX/RX descriptor rings
- **MAC**: read from EEPROM via EERD register at init
- **Interrupts**: ICR self-clearing; IRQ routed through 8259A PIC
- **Used by**: VirtualBox (Intel PRO/1000 MT Desktop) and QEMU (`-device e1000`)

#### NetDevice Abstraction (`net_device.h`, `net_device.c`)

Both drivers are registered behind a `NetDevice` interface struct:

```c
typedef struct NetDevice {
    const char *name;
    int  (*init)(struct NetDevice *dev);
    void (*get_mac)(struct NetDevice *dev, uint8_t *buf);
    int  (*send)(struct NetDevice *dev, const uint8_t *data, uint16_t len);
    void (*poll)(struct NetDevice *dev);
    void (*set_rx_callback)(struct NetDevice *dev, netdev_rx_fn cb);
    void (*setup_irq)(struct NetDevice *dev);
    void *priv;
} NetDevice;
```

`netdev_probe()` (called by `net_stack_init_ex()`) tries e1000 first, then
virtio-net. All TCP/IP stack layers call only the `netdev_*` wrappers —
they never reference either driver directly.

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
| `exec.library` | v45 | 0x00000004 | Process management, memory allocation, signals, IPC |
| `utility.library` | v37 | 0x00000050 | String functions, memory utilities |
| `console.device` | v40 | 0x00000060 | Console I/O |
| `mathffp.library` | v40 | 0x00000060 | Software floating-point (uses softfloat) |
| `locale.library` | v38 | 0x000000B0 | Localization support |
| `ixemul.library` | v53 | 0x00000090 | Unix compatibility layer |
| `graphics.library` | v40 | 0x000000C0 | Graphics primitives |
| `dos.library` | v40 | 0x000000D0 | File system operations |
| `bsdsocket.library` | v4 | 0x00003000 | BSD socket API mapped to native TCP/IP stack |
| `workbench.library` | v45 | 0x00000000 | Workbench desktop integration |
| `intuition.library` | v40 | 0x00005000 | Intuition GUI API |

#### Devices

| Device | Version | Base Address | Connected To |
|--------|---------|-------------|-------------|
| `timer.device` | v40 | 0x000000A0 | RTC driver (CMOS) |
| `keyboard.device` | v40 | 0x000000B0 | PS/2 keyboard driver |

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

- **exec.library**: Task/process control, signals, IPC, AllocMem/FreeMem, OpenLibrary/CloseLibrary, FindTask
- **utility.library**: String functions (StrIcmp, StrNicmp, UcStr, LcStr) with implementation logic
- **mathffp.library**: All 18 floating-point functions with softfloat integration logic
- **dos.library**: VFS-backed Lock/Unlock, Examine/ExamineNext, file I/O
- **timer.device**: Connected to RTC driver for timing functions
- **keyboard.device**: Connected to PS/2 keyboard driver for input
- **bsdsocket.library**: Full BSD socket API mapped to the native TCP/IP stack
- **workbench.library**: Workbench startup and icon integration stubs
- **intuition.library**: Window/open/close gadget stubs

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

### Additional Windows

Besides the main shell and Workbench, the kernel provides several standalone
windows that can be opened from the shell or from M68k/Ring-3 programs:

| Window | Source | Opened by |
|--------|--------|-----------|
| About UAOS | `kernel/display/about_win.c` | Workbench menu |
| Calculator | `kernel/display/calc_win.c` | `calculator` |
| Clock | `kernel/display/clock_win.c` | `clock` |
| NetInfo | `kernel/display/netinfo_win.c` | `netinfo` |
| Pointer preferences | `kernel/display/pointer_prefs.c` | `pointer` |
| User window / Guide viewer | `kernel/display/user_window.c` | `Guide` (userspace) |
| VIM editor | `kernel/display/vim_win.c` | `vim` |
| File browser | `kernel/display/filebrowser.c` | Workbench icon double-click |

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
| `BlockDev_RegisterPartition(parent, idx, start, count, name)` | Register a partition device |
| `BlockDev_UnregisterPartitions(parent)` | Remove all partitions of a parent |
| `BlockDev_CheckFormatted(dev)` | Detect valid boot sector / FAT BPB |
| `BlockDev_ReadVolLabel(dev, buf, max)` | Read FAT32 volume label from boot sector |
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
| `VFS_MountPartition(name)` | Mount a partition volume by name (creates RAMFS backing) |
| `VFS_GetMountCount()` | Return number of mounted volumes |
| `VFS_GetMountName(idx, dst, max)` | Get name of i-th mounted volume |

### RAM Filesystem (`ramfs.c`)

In-memory filesystem with BSS-backed storage:

- **Node tree**: 256 nodes maximum
- **Data pool**: 64KB for file contents
- **Auto-mounted at boot**: RAM: with T, ENV, CLIPS, S directories
- **Partition volumes**: Auto-mounted at boot if formatted; mountable by display name (e.g. `DH0:`) or FAT32 volume label (e.g. `WORK:`)
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

**Status**: Boot sector parsing, volume label extraction (`BlockDev_ReadVolLabel`), and formatting via `FAT32_Format()` are implemented. Cluster-chain read/write and directory traversal are pending.

#### ISO9660 (`iso9660.c`)

CD-ROM reader used at boot to load files from the boot ISO into RAMFS:

- **Operations**: `ISO9660_MountCD()` scans the CD, reads directory entries, and copies files into RAMFS nodes
- **Used by**: boot sequence to populate the `Workbench:` volume from the ISO

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
| `dir [path]` | List files in current directory |
| `cd [path]` | Change or show current directory |
| `makedir <path>` | Create a directory |
| `delete <path>` | Delete a file or empty directory |
| `type <file>` | Display file contents |
| `copy <src> <dst>` | Copy a file |
| `rename <from> <to>` | Rename or move a file |
| `pwd` | Print working directory (native userspace utility) |
| `find [path] [-name pat] [-type f|d]` | Recursively list directory contents (native userspace utility) |
| `file <path>` | Identify file format using magic numbers (native userspace utility) |
| `strings <path> [-n len]` | Scan files for printable character sequences (native userspace utility) |
| `hello` | Print simple greeting (userspace validation utility) |
| `echo <text>` | Print text to shell |
| `protect <flags> <path>` | Set file attributes (`+r`, `-r`, `+h`, `-h`) |
| `attr <path>` | Show file attributes (Read-Only, Hidden, etc.) |
| `info [device]` | Show mounted disks and volumes; or info for a specific device |
| `alias [name cmd]` | Create or list command aliases |
| `unalias <name>` | Remove an alias |
| `set [name val]` | Set or list environment variables |
| `unset <name>` | Remove an environment variable |
| `date` | Show current date and time |
| `which <cmd>` | Locate a command |
| `disks` | List detected block devices |
| `fdisk <device>` | Partition a block device |
| `format <dev> [fs]` | Format a partition (FAT32) |
| `run <cmd> [args]` | Run a command in a new CLI |

### Current Implementation Status

- **Block device layer**: Complete with VirtIO registration and MBR partition support
- **VFS layer**: Complete with RAM filesystem and partition volume mounting
- **FAT32**: Boot sector parsing, volume label extraction, and formatting implemented; file read/write via virtqueue I/O pending
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

### Command Execution Flow

The shell resolves a command in this order:

1. **Shell built-ins** — implemented directly in the shell (`help`, `cd`, `alias`, etc.).
2. **Resident commands** — kept in a 256 KB in-memory cache by `resident`.
3. **Native C: commands** — dispatched through the `k_native_cmds[]` table in `kernel/shell/native_cmd.c`.
4. **M68k Hunk binaries** — loaded and run via the Musashi emulator.
5. **Ring-3 userspace ELF64 binaries** — loaded and run with `INT 0x80` syscalls.

All lookup is case-insensitive. Native commands may declare an AmigaDOS-style template (`/A` required, `/K` keyword, `/S` switch, `/N` numeric, `/M` multiple, `/F` free-form) that is parsed automatically before the handler is invoked.

### Shell Built-ins

| Command | Description |
|---------|-------------|
| `help` | List available commands |
| `cd [path]` | Change or show current directory |
| `alias [name cmd]` | Create or list command aliases |
| `unalias <name>` | Remove an alias |
| `set [name val]` | Set or list local variables |
| `unset <name>` | Remove a local variable |
| `path [dirs...]` | Show or set the command search path |
| `setenv <name> <value>` | Set a global environment variable |
| `unsetenv <name>` | Remove a global environment variable |
| `showconfig` | Show hardware configuration |

### Native C: Commands

| Command | Description |
|---------|-------------|
| `version` | Show kernel version and architecture |
| `mem` | Display memory information |
| `libs` | List loaded ROM libraries with versions |
| `clear` | Clear the shell history buffer |
| `reboot` | Reboot the system |
| `dir [path]` | List directory contents |
| `makedir <path>` | Create a directory |
| `delete <path>` | Delete a file or empty directory |
| `type <file>` | Display file contents |
| `copy <src> <dst>` | Copy a file |
| `rename <from> <to>` | Rename or move a file |
| `echo <text>` | Print text to shell |
| `protect <flags> <path>` | Set file attributes (`+r`, `-r`, `+h`, `-h`) |
| `attr <path>` | Show file attributes |
| `info [device]` | Show mounted disks and volumes |
| `date` | Show current date and time |
| `which <cmd>` | Locate a command |
| `disks` | List detected block devices |
| `fdisk <device>` | Partition a block device |
| `format <dev> [fs]` | Format a partition (FAT32) |
| `pointer` | Open pointer preferences |
| `run <cmd> [args]` | Run a command in a new CLI |
| `assign [name: target]` | Create or list assigns |
| `execute <script>` | Execute a script file |
| `loadwb` | Launch the Workbench desktop |
| `calculator` | Open the calculator window |
| `clock` | Open the clock window |
| `ifconfig [dhcp \| <ip> <gw>]` | Show or configure network |
| `ping <host> [count]` | Send ICMP echo requests |
| `route` | Display routing table and ARP cache |
| `nslookup <host> [server]` | Resolve a hostname via DNS |
| `ntpd [server]` | Synchronise time via NTP |
| `netstart` / `netstop` | Start or stop the network stack |
| `netinfo` | Open the network information window |
| `grep [-i] <pattern> <file>` | Search a file for a pattern |
| `more <file>` | Paginated file viewer |
| `vim <file>` | Inline text editor |
| `newcli` / `newshell` | Open a new shell window |
| `ask <prompt>` | Prompt the user for input |
| `resident` | Manage resident commands |
| `ps` | List running tasks |
| `list` | List files with details |
| `search <pattern> [file]` | Advanced file search |
| `sort [file] [options]` | Sort file lines |
| `join <file1> <file2>` | Join two files by key |
| `wait` | Wait for background jobs |
| `prompt <string>` | Set a custom shell prompt |
| `stack` | Show stack usage |
| `why` | Show last command return code |
| `failat <n>` | Set failure threshold |
| `quit [rc]` | Exit a script |
| `endcli` | Close the current shell window |
| `filenote <file> <comment>` | Set a file comment |
| `relabel <device> <name>` | Rename a volume |
| `avail` | Show available memory |
| `getenv <name>` | Read an environment variable |
| `unset <name>` | Remove an environment variable |
| `jobs` | List background jobs |
| `install <device>` | Install a boot block |
| `diskchange <device>` | Notify disk change |
| `addbuffers <device> <n>` | Add disk buffers |
| `requestchoice <title> <body> <buttons...>` | Choice dialog |
| `requestfile [options]` | File requester dialog |
| `changetaskpri <pri> [task]` | Change task priority |
| `status` | Show system status |

### Ring-3 Userspace Commands

These commands are built from `system/userspace/` as native x86-64 ELF64 binaries and communicate with the kernel via `INT 0x80` syscalls:

| Command | Description |
|---------|-------------|
| `hello` | Print a simple greeting |
| `pwd` | Print working directory |
| `file <path>...` | Identify file format from magic numbers |
| `strings <path>... [-n minlen]` | Extract printable strings |
| `find [path] [-name pat] [-type f\|d]` | Recursively search directories |
| `Guide` | AmigaGuide help viewer (`Tools:Guide`) |

---

## TCP/IP Networking

UAOS includes a complete freestanding TCP/IP stack. It auto-detects the
network card at boot, attempts DHCP, and falls back to a static address if
DHCP times out. The stack runs in the kernel event loop — no threads or
blocking I/O required.

### Architecture

```
Shell commands / bsdsocket.library
          │
          ▼
   icmp / tcp / udp  (kernel/net/)
          │
          ▼
    ip.c  ◄──►  arp.c
          │
          ▼
   net_device  (abstract hardware interface)
          │
     ┌────┴────┐
     ▼         ▼
 virtio_net  e1000
 (QEMU)    (VirtualBox)
```

### Initialisation

`net_stack_init()` (or `net_stack_init_ex()`) is called once from
`uaos_kernel_main()`. It:

1. Calls `netdev_probe()` — scans PCI for e1000 first, then virtio-net.
2. Calls the found driver's `init()` to reset and configure the hardware.
3. Attempts DHCP (1 second timeout by default).
4. Falls back to static `10.0.2.15/24 gw 10.0.2.2` if DHCP fails.
5. Initialises ARP and IP tables with the assigned address.
6. Registers the RX callback and IRQ handler.

```c
/* Static IP, skip DHCP */
net_stack_init(IPV4(10,0,2,15), IPV4(10,0,2,2), IPV4(255,255,255,0));

/* DHCP with 2-second timeout, static fallback */
net_stack_init_ex(IPV4(10,0,2,15), IPV4(10,0,2,2),
                  IPV4(255,255,255,0), 2000);
```

### Stack Public API (`kernel/net/stack.h`)

| Function | Description |
|---|---|
| `net_stack_init(ip, gw, nm)` | Init with static fallback; tries DHCP first |
| `net_stack_init_ex(ip, gw, nm, ms)` | Init with explicit DHCP timeout |
| `net_stack_is_up()` | Returns 1 if a network card was found and stack is live |
| `net_stack_dhcp_used()` | Returns 1 if the current IP came from DHCP |
| `net_stack_poll()` | Process pending RX packets (call from event loop) |
| `net_stack_tick()` | TCP retransmit/timeout tick (call from PIT handler) |
| `net_stack_get_ip()` | Returns the current IPv4 address |
| `net_ip_to_str(ip, buf)` | Formats an `ipv4_t` as `"a.b.c.d"` into `buf` (≥18 bytes) |
| `net_str_to_ip(str, out)` | Parses `"a.b.c.d"` into an `ipv4_t`; returns 0 on error |

### IPv4 Address Type

```c
typedef uint32_t ipv4_t;   /* host byte order: 0xAABBCCDD = a.b.c.d */

/* Build address from dotted quads */
#define IPV4(a,b,c,d) ((ipv4_t)(((uint32_t)(a)<<24)|((uint32_t)(b)<<16) \
                               |((uint32_t)(c)<<8)|(uint32_t)(d)))
```

### ICMP — Ping (`kernel/net/icmp.h`)

```c
/* Send one ICMP echo request */
void icmp_ping(ipv4_t dst_ip, uint16_t seq);

/* Poll for reply after icmp_ping() */
int  icmp_got_reply(void);   /* returns 1 when reply arrives */
void icmp_clear_reply(void); /* reset before each icmp_ping() */
```

The shell `ping` command uses these functions, yielding every 100 ms so
the UI remains responsive during the 1-second per-echo timeout.

### ARP (`kernel/net/arp.h`)

The ARP module maintains a 16-entry cache. `ip_send()` consults it
automatically; callers do not need to manage ARP directly.

```c
void arp_init(ipv4_t my_ip, const uint8_t *my_mac);
void arp_request(ipv4_t target_ip);          /* broadcast ARP who-has */
int  arp_lookup(ipv4_t ip, uint8_t *mac_out);/* 1 = found, 0 = miss   */
void arp_cache_update(ipv4_t ip, const uint8_t *mac);
```

### IP (`kernel/net/ip.h`)

```c
#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP   17

int  ip_send(ipv4_t dst_ip, uint8_t proto,
             uint8_t *payload, uint16_t payload_len);

ipv4_t ip_get_local(void);
ipv4_t ip_get_gateway(void);
ipv4_t ip_get_netmask(void);
```

`ip_send()` looks up the nexthop (gateway if off-subnet), resolves its
MAC via ARP (returns 0 if not yet cached — caller retries after
`arp_request()`), and hands the frame to `netdev_send()`.

### UDP (`kernel/net/udp.h`)

```c
int  udp_open(uint16_t local_port);  /* 0 = ephemeral */
void udp_close(int sock);
int  udp_send(int sock, ipv4_t dst_ip, uint16_t dst_port,
              const uint8_t *data, uint16_t len);
int  udp_recv(int sock, uint8_t *buf, uint16_t maxlen,
              ipv4_t *src_ip_out, uint16_t *src_port_out);
```

`udp_recv()` is non-blocking and returns 0 if no datagram is available.
Up to 8 sockets may be open simultaneously. Each socket has a 2 KB
receive ring buffer.

**Example — send a UDP datagram:**

```c
int s = udp_open(0);          /* bind to ephemeral port */
uint8_t msg[] = "hello";
udp_send(s, IPV4(10,0,2,2), 9, msg, sizeof(msg));
udp_close(s);
```

### TCP (`kernel/net/tcp.h`)

Up to 8 simultaneous TCP connections. Each socket has 4 KB TX and 4 KB
RX ring buffers. `tcp_tick()` must be called periodically (typically
from the PIT IRQ handler) for retransmit and timeout handling.

#### Client connection

```c
int sock = tcp_connect(IPV4(93,184,216,34), 80, 0);
/* wait until established */
while (tcp_state(sock) != TCP_ESTABLISHED) net_stack_poll();

uint8_t req[] = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
tcp_send(sock, req, sizeof(req) - 1);

uint8_t buf[512]; int n;
while ((n = tcp_recv(sock, buf, sizeof(buf))) > 0) {
    /* process buf[0..n-1] */
    net_stack_poll();
}
tcp_close(sock);
```

#### Server (passive listen)

```c
int lsock = tcp_listen(8080);
while (1) {
    net_stack_poll();
    int csock = tcp_accept(lsock);
    if (csock < 0) continue;
    /* handle csock … */
    tcp_close(csock);
}
```

#### TCP API reference

| Function | Description |
|---|---|
| `tcp_connect(ip, port, lport)` | Active open; returns socket index or -1 |
| `tcp_listen(port)` | Passive open; returns listening socket index or -1 |
| `tcp_accept(lsock)` | Accept a pending connection; returns new socket or -1 |
| `tcp_send(sock, data, len)` | Queue data for sending; returns bytes queued |
| `tcp_recv(sock, buf, max)` | Non-blocking receive; returns bytes read or 0 |
| `tcp_close(sock)` | Initiate graceful close (sends FIN) |
| `tcp_state(sock)` | Returns current `TcpState` enum value |
| `tcp_tick()` | Retransmit / timeout processing (call from timer IRQ) |

#### TCP connection states

| State | Meaning |
|---|---|
| `TCP_CLOSED` | Socket unused |
| `TCP_SYN_SENT` | SYN sent, waiting for SYN-ACK |
| `TCP_ESTABLISHED` | Data transfer in progress |
| `TCP_CLOSE_WAIT` | Remote FIN received, waiting for app to close |
| `TCP_FIN_WAIT_1/2` | Local FIN sent, draining |
| `TCP_TIME_WAIT` | 2×MSL quiet time before socket recycled |

### DHCP (`kernel/net/dhcp.h`)

DHCP is used automatically by `net_stack_init_ex()`. It can also be
called directly:

```c
DhcpLease lease;
if (dhcp_request(&lease, 2000)) {   /* 2-second timeout */
    /* lease.ip, lease.gateway, lease.netmask, lease.dns populated */
}
```

DHCP sends DISCOVER/REQUEST over UDP port 67 and listens on port 68.

### Byte-Order Helpers (`kernel/net/net.h`)

```c
uint16_t net_htons(uint16_t v);   /* host → network (16-bit) */
uint16_t net_ntohs(uint16_t v);   /* network → host (16-bit) */
uint32_t net_htonl(uint32_t v);   /* host → network (32-bit) */
uint32_t net_ntohl(uint32_t v);   /* network → host (32-bit) */
```

### Polling Model

The stack has no dedicated thread. RX is processed in two ways:

1. **IRQ path**: the NIC fires an interrupt, the handler calls
   `e1000_poll()` or `virtio_net_poll()`, which invokes the RX callback,
   which calls `eth_rx()` → `arp_rx()` / `ip_rx()` → TCP/UDP demux.
2. **Poll path**: `net_stack_poll()` is called from the main event loop
   (`hlt` loop) and from `CMD_YIELD()` inside long-running shell commands.

Both paths call `netdev_poll()` which is guarded by a reentrancy lock, so
it is safe to call from both contexts.

### Network Configuration Files

| File | Purpose | Default |
|------|---------|---------|
| `S:net.conf` | Network mode (`dhcp` or `static`), IP, netmask, gateway, DNS | `dhcp` / `10.0.2.15/24 gw 10.0.2.2 dns 8.8.8.8` |
| `S:ntp.conf` | NTP server hostname | `pool.ntp.org` |
| `S:timezone.conf` | IANA timezone name | `Australia/Sydney` |

`netstart` reads `S:net.conf` and initialises the stack. `ntpd` reads `S:ntp.conf` and `S:timezone.conf` for time synchronisation and local-time conversion.

### `bsdsocket.library` (AmigaOS Socket API)

The `bsdsocket.library` ROM module (v4, base 0x3000) exposes a standard AmigaOS
BSD socket LVO table to M68k binaries. The native implementation maps each call
to the kernel TCP/IP stack.

| LVO Offset | Function | Args (M68k) |
|------------|----------|-------------|
| -30 | `socket` | `domain/D0, type/D1, protocol/D2` |
| -36 | `bind` | `fd/D0, sockaddr/A0, addrlen/D1` |
| -42 | `listen` | `fd/D0, backlog/D1` |
| -48 | `accept` | `fd/D0, sockaddr/A0, addrlen/A1` |
| -54 | `connect` | `fd/D0, sockaddr/A0, addrlen/D1` |
| -60 | `send` | `fd/D0, buf/A0, len/D1, flags/D2` |
| -66 | `sendto` | `fd/D0, buf/A0, len/D1, flags/D2, addr/A1, addrlen/D3` |
| -72 | `recv` | `fd/D0, buf/A0, len/D1, flags/D2` |
| -78 | `recvfrom` | `fd/D0, buf/A0, len/D1, flags/D2, addr/A1, addrlen/A2` |
| -84 | `closesocket` | `fd/D0` |
| -96 | `setsockopt` | `fd/D0, level/D1, optname/D2, val/A0, optlen/D3` |
| -102 | `getsockopt` | `fd/D0, level/D1, optname/D2, val/A0, optlen/A1` |
| -108 | `IoctlSocket` | `fd/D0, req/D1, arg/A0` |
| -132 | `inet_addr` | `str/A0` |
| -138 | `inet_ntoa` | `addr/D0` |
| -210 | `gethostbyname` | `name/A0` |

Supported constants: `AF_INET = 2`, `SOCK_STREAM = 1`, `SOCK_DGRAM = 2`,
`IPPROTO_TCP = 6`, `IPPROTO_UDP = 17`. Socket descriptors 0–7 map to TCP
sockets, 8–15 to UDP sockets.

### VirtualBox Setup

1. VM Settings → Network → Adapter 1
2. **Attached to**: NAT (default)
3. **Adapter Type**: Intel PRO/1000 MT Desktop (82540EM)
4. Boot UAOS — the e1000 driver is detected automatically.
5. `ping 10.0.2.2` (VirtualBox gateway) confirms basic connectivity.
6. `ping 8.8.8.8` tests full NAT routing via the host.

### QEMU Setup

```bash
bash scripts/run_with_disk.sh
# or manually:
qemu-system-x86_64 ... \
  -netdev user,id=n0 \
  -device virtio-net-pci,netdev=n0,disable-modern=on
```

The virtio-net driver is selected automatically. QEMU's slirp user-mode
network responds to `ping 10.0.2.2` (gateway). Pings to external IPs
require the host process to have `CAP_NET_RAW` (or use TAP/bridge mode);
all TCP and UDP traffic is proxied normally.

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
| 1 | Creates `build/` staging directories and the dynamic `SYS_ROOT` image |
| 2 | Builds host tools (`gen_uaos_native`, `gen_uaos_m68k`, `gen_uaos_x64`, `gen_m68k_library`) |
| 3 | Assembles `uaos_kernel_entry.asm`, `idt_stubs.asm` and `task_switch.asm` with NASM |
| 4 | Generates the Musashi M68k opcode table if needed |
| 5 | Compiles all C sources: `-ffreestanding -m64 -O2 -std=c11 -fno-stack-protector` |
| 6 | Links into `uaos-kernel.elf` (ELF64) via the custom linker script |
| 7 | Wraps embedded M68k binaries from `emulation/binaries/` and Amiga `.library` files |
| 8 | Builds native x86-64 Ring-3 userspace programs from `system/userspace/` |
| 9 | Stages the `system/` skeleton into `SYS_ROOT` (C:, S:, LIBS:, DEVS:, L:, SYS, Tools) |
| 10 | Injects `grub.cfg` and the AROS kickstart configuration |
| 11 | Produces hybrid BIOS+EFI ISO with `grub-mkrescue` |

### GCC Flags

```
-ffreestanding -fno-stack-protector -fno-pie -fno-PIE
-fno-asynchronous-unwind-tables -mno-red-zone -nostdlib
-m64 -O2 -std=c11 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
-Wall -Wextra -Wno-unused-function -Wno-unused-variable
-Wno-unused-parameter -Wno-address-of-packed-member
-Wno-missing-braces
```

---

## Running in QEMU

### OVMF Setup

Copy a **fresh** variables file before each run. Stale vars can save a changed
boot order and cause the firmware to drop to the UEFI shell instead of booting
from CD:

```bash
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/ovmf_vars.fd
```

### Launch Command

```bash
qemu-system-x86_64 \
  -machine q35,usb=off \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=/tmp/ovmf_vars.fd \
  -device piix3-ide,id=ide \
  -drive if=none,id=cdrom,media=cdrom,file=build/Ultimate_Amiga_OS.iso \
  -device ide-cd,drive=cdrom,bus=ide.0 \
  -m 512M \
  -vga virtio \
  -no-reboot \
  -no-shutdown
```

| Flag | Purpose |
|---|---|
| `-machine q35,usb=off` | Q35 chipset; `usb=off` prevents USB tablet conflicting with PS/2 mouse |
| `OVMF_CODE_4M.fd` | UEFI firmware (read-only) |
| `ovmf_vars.fd` | UEFI variable store (writable copy) |
| `-device piix3-ide` | Explicit IDE controller (Q35 has no built-in IDE; required for ATAPI CD-ROM) |
| `-device ide-cd` | Attach CD-ROM to the IDE controller |
| `-vga virtio` | Best framebuffer performance |
| `-no-reboot` | Keeps QEMU open after kernel reboot call |
| `-no-shutdown` | Keeps QEMU window open when guest CPU is idle |

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
