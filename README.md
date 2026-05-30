# Ultimate Amiga OS (UAOS)

A bare-metal x86_64 hobby operating system inspired by the Amiga Workbench 3.x aesthetic, built from scratch using NASM, C11, GRUB2, and OVMF/UEFI.

UAOS boots directly from a hybrid ISO via GRUB2 Multiboot2, initialises a linear framebuffer, and presents a graphical Workbench-style desktop with a window manager, PS/2 mouse and keyboard support, and an interactive shell.

---

## Features

- **Workbench-style desktop** — stipple backdrop, menu bar, status bar, disk icons
- **Window manager** — multiple windows, click-to-focus, z-order, title bar drag, resize grip
- **PS/2 mouse** — IRQ12-driven relative tracking, 16×16 Amiga-style software cursor
- **PS/2 keyboard** — IRQ1-driven, scancode set 1, ring buffer
- **Shell window** — scrollable history, input line, built-in commands: `help`, `version`, `mem`, `clear`, `reboot`, `libs`, `dir`, `cd`, `makedir`, `delete`, `type`, `copy`
- **IDT / 8259A PIC** — 256-vector IDT, PIC remapped to vectors 32–47
- **MMU sandbox** — 4-level paging, 2 MB huge pages
- **M68k emulation** — Musashi CPU, ILLEGAL opcode dispatch, LVO stubs
- **ROM module system** — Native AmigaOS-compatible library implementations:
  - `exec.library` v45 — Process management, memory allocation
  - `utility.library` v37 — String functions, memory utilities
  - `console.device` v40 — Console I/O
  - `mathffp.library` v40 — Floating-point operations
  - `locale.library` v38 — Localization support
  - `ixemul.library` v53 — Unix compatibility layer
  - `timer.device` v40 — Timing functions (connected to RTC)
  - `keyboard.device` v40 — Keyboard input (connected to PS/2 driver)
  - `graphics.library` v40 — Graphics primitives
  - `dos.library` v40 — File system operations
- **VFS / RAM filesystem** — In-memory node tree, auto-mounted at boot with T, ENV, CLIPS, S dirs
- **VirtIO block device driver** — PCI scanning, device detection, capacity reporting
- **Block device layer** — Unified interface for storage devices
- **RTC driver** — CMOS real-time clock with UIE interrupt
- **EFI + BIOS hybrid ISO** — boots on OVMF UEFI and legacy BIOS via GRUB2

---

## Repository Layout

```
uaos/
├── kernel/
│   ├── boot/           # NASM entry point, C kernel main, linker script
│   ├── display/        # Framebuffer, desktop, cursor, window manager, shell window
│   ├── irq/            # IDT, 8259A PIC, PS/2 mouse, PS/2 keyboard, VMware mouse, RTC, VirtIO block
│   ├── exec/           # Thunk handler, MMU sandbox, page fault ISR, ROM modules
│   │                   # ROM libraries: utility, console, mathffp, locale, ixemul
│   │                   # ROM devices: timer, keyboard, graphics
│   │                   # dos.library implementation
│   └── dos/            # VFS layer, RAM filesystem, block device layer
├── emulation/
│   ├── rom_patches/    # M68k Vasm/Devpac stubs, kickstart config
│   ├── src/musashi/    # M68k CPU emulator
│   ├── uaos_m68k_glue.c # M68k emulator glue, LVO stubs, DOS stubs
│   └── uaos_emu_registry.c
├── drivers/            # Future device drivers
├── scripts/
│   ├── build_iso.sh    # Full build pipeline
│   └── grub.cfg        # GRUB2 multiboot2 configuration
├── sys-root/           # Amiga-style filesystem skeleton (C, DEVS, L, S, SYS)
├── documentation/
│   ├── uaos.guide      # AmigaGuide database
│   └── manual.tex      # LaTeX technical reference
└── build/              # Generated output (created by build script)
    └── Ultimate_Amiga_OS.iso
```

---

## Dependencies

Install on Debian/Ubuntu:

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

---

## Building the ISO

From the repository root:

```bash
bash scripts/build_iso.sh
```

To do a clean rebuild from scratch:

```bash
bash scripts/build_iso.sh --clean
```

On success the ISO is written to:

```
build/Ultimate_Amiga_OS.iso
```

### What the build script does

| Step | Action |
|------|--------|
| 1 | Creates `build/` staging directories |
| 2 | Assembles `uaos_kernel_entry.asm` and `idt_stubs.asm` with NASM |
| 3 | Compiles all C kernel sources with GCC (`-ffreestanding -m64 -O2 -std=c11`) |
| 4 | Links everything into `uaos-kernel.elf` (ELF64) via the custom linker script |
| 5 | Packages the `sys-root` Amiga filesystem skeleton |
| 6 | Injects `grub.cfg` |
| 7 | Builds a standalone GRUB EFI image (`bootx64.efi`) with `grub-mkstandalone` |
| 8 | Produces a hybrid BIOS+EFI ISO with `grub-mkrescue` |

---

## Running in QEMU

### First-time setup — copy OVMF variables file

OVMF requires a writable variables file. Copy it once:

```bash
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/ovmf_vars.fd
```

> If your OVMF package uses a different path, check with:
> `find /usr/share -name 'OVMF_VARS_4M.fd' 2>/dev/null`

### Launch QEMU

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

| Flag | Reason |
|------|--------|
| `-machine q35,usb=off` | Q35 chipset; `usb=off` disables USB tablet which conflicts with PS/2 mouse |
| `-drive if=pflash ...OVMF_CODE` | UEFI firmware (read-only) |
| `-drive if=pflash ...ovmf_vars` | UEFI variable store (writable copy) |
| `-vga virtio` | Best framebuffer performance under QEMU |
| `-no-reboot` | Keeps QEMU open if the kernel calls reboot (useful for debugging) |

### Optional: serial debug output

Add `-serial stdio` to see serial debug output from the kernel on your terminal:

```bash
qemu-system-x86_64 \
  -machine q35,usb=off \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=/tmp/ovmf_vars.fd \
  -cdrom build/Ultimate_Amiga_OS.iso \
  -m 512M \
  -vga virtio \
  -no-reboot \
  -serial stdio
```

Or log to a file:

```bash
  -serial file:/tmp/uaos_serial.log
```

---

## Using the Desktop

Once booted you will see a Workbench-style desktop.

### Mouse

| Action | Result |
|--------|--------|
| Move mouse | Cursor follows |
| Click title bar | Focus and raise window |
| Drag title bar | Move window (can extend off screen edges) |
| Drag resize grip (bottom-right corner) | Resize window |

> QEMU captures the mouse when you click inside the window. Press **Ctrl+Alt+G** to release it.

### Shell window

Click the **UAOS Shell** title bar to focus it, then type commands:

| Command | Description |
|---------|-------------|
| `help` | List available commands |
| `version` | Show kernel version and architecture |
| `mem` | Display memory information |
| `clear` | Clear the shell history |
| `reboot` | Reboot the system |
| `libs` | List loaded kernel libraries with versions |
| `dir` | List files in current directory |
| `cd` | Change current directory |
| `makedir` | Create a directory |
| `delete` | Delete a file or directory |
| `type` | Display file contents |
| `copy` | Copy a file |

---

## Architecture Overview

```
GRUB2 Multiboot2
    └── uaos_kernel_entry.asm   (32-bit protected → 64-bit long mode)
            └── uaos_kernel_main.c
                    ├── FB_Init()           framebuffer from Multiboot2 tag
                    ├── IDT_Init()          256-vector IDT + 8259A PIC remap
                    ├── PS2Mouse_Init()     IRQ12 PS/2 mouse driver
                    ├── PS2Kbd_Init()       IRQ1  PS/2 keyboard driver
                    ├── RTC_Init()          CMOS real-time clock (IRQ8)
                    ├── VFS_Init()          VFS layer + RAM filesystem
                    ├── BlockDev_Init()     Block device layer
                    ├── virtio_blk_init()   VirtIO block device driver
                    ├── UAOS_ROM_RegisterAll()  Register ROM modules
                    │   ├── exec.library v45
                    │   ├── utility.library v37
                    │   ├── console.device v40
                    │   ├── mathffp.library v40
                    │   ├── locale.library v38
                    │   ├── ixemul.library v53
                    │   ├── timer.device v40 (→ RTC)
                    │   ├── keyboard.device v40 (→ PS/2)
                    │   ├── graphics.library v40
                    │   └── dos.library v40 (→ VFS)
                    ├── Desktop_Draw()      Workbench backdrop + icons
                    ├── ShellWin_Init()     Shell window → registers with WM
                    └── event loop
                            ├── WM_MouseEvent()   drag / focus / resize
                            └── WM_KeyEvent()     routes keystrokes to focused window
```

---

## Known Limitations

- M68k emulation has basic LVO stubs but needs full memory access integration
- ROM library functions are stubs with implementation logic in comments
- VirtIO block device read/write needs virtqueue I/O implementation
- No filesystem support on block devices (FAT32, ext2, etc.)
- No networking, no audio
- Single CPU, no SMP
- Clock display updates via RTC but needs full date/time integration

---
<img width="1413" height="1069" alt="image" src="https://github.com/user-attachments/assets/66433a49-276c-4408-a7e7-6b869220c57e" />

## License

This project is a personal research and hobby project. No licence is currently applied.
