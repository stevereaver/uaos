# Ultimate Amiga OS (UAOS)

A bare-metal x86_64 hobby operating system inspired by the Amiga Workbench 3.x aesthetic, built from scratch using NASM, C11, GRUB2, and OVMF/UEFI.

UAOS boots directly from a hybrid ISO via GRUB2 Multiboot2, initialises a linear framebuffer, and presents a graphical Workbench-style desktop with a window manager, PS/2 mouse and keyboard support, and an interactive shell.

---

## Features

- **Workbench-style desktop** — stipple backdrop, menu bar, status bar, disk icons
- **Window manager** — multiple windows, click-to-focus, z-order, title bar drag, resize grip
- **PS/2 mouse** — IRQ12-driven relative tracking, 16×16 Amiga-style software cursor
- **PS/2 keyboard** — IRQ1-driven, scancode set 1, ring buffer
- **Shell window** — scrollable history, input line, built-in commands: `help`, `cd`, `alias`, `unalias`, `set`, `unset`, `path`, `setenv`, `unsetenv`, `showconfig`
- **Native C: commands** — 65+ x86-64 kernel commands including `version`, `mem`, `libs`, `dir`, `makedir`, `delete`, `type`, `copy`, `rename`, `echo`, `protect`, `attr`, `info`, `date`, `which`, `disks`, `fdisk`, `format`, `assign`, `execute`, `loadwb`, `run`, `ping`, `ifconfig`, `route`, `nslookup`, `ntpd`, `netstart`, `netstop`, `netinfo`, `grep`, `more`, `list`, `search`, `sort`, `ps`, `jobs`, `wait`, `ask`, `newcli`, `calculator`, `clock`, `pointer`, `vim`, `status`, `avail`, `filenote`, `relabel`, `install`, `diskchange`, `addbuffers`, `requestchoice`, `requestfile`, `changetaskpri`, `prompt`, `stack`, `why`, `failat`, `quit`, `endcli`, `getenv`, `unset`, `resident`, `join`
- **Ring-3 userspace programs** — `hello`, `pwd`, `file`, `strings`, `find`, `Guide` built as native x86-64 ELF64 binaries using the `INT 0x80` syscall interface
- **IDT / 8259A PIC** — 256-vector IDT, PIC remapped to vectors 32–47, plus local APIC setup
- **MMU sandbox** — 4-level paging, 2 MB huge pages
- **M68k emulation** — Musashi CPU, ILLEGAL opcode dispatch, LVO stubs; runs raw Amiga Hunk binaries and embedded M68k binaries
- **ROM module system** — Native AmigaOS-compatible library implementations:
  - `exec.library` v45 — Process management, memory allocation, signals, IPC
  - `utility.library` v37 — String functions, memory utilities
  - `console.device` v40 — Console I/O
  - `mathffp.library` v40 — Floating-point operations
  - `locale.library` v38 — Localization support
  - `ixemul.library` v53 — Unix compatibility layer
  - `timer.device` v40 — Timing functions (connected to RTC)
  - `keyboard.device` v40 — Keyboard input (connected to PS/2 driver)
  - `graphics.library` v40 — Graphics primitives
  - `dos.library` v40 — File system operations
  - `bsdsocket.library` v4 — BSD socket API mapped to the native TCP/IP stack
  - `workbench.library` v45 — Workbench desktop integration
  - `intuition.library` v40 — Intuition GUI API
- **VFS / RAM filesystem** — In-memory node tree (1024 nodes, 512 KB per file), auto-mounted at boot with T, ENV, CLIPS, S dirs; partition volumes mountable by display name or FAT32 volume label
- **Filesystem drivers** — FAT32, PFS3, EXT4 (read-only), ISO9660 (CD-ROM), and RAMFS
- **VirtIO block device driver** — PCI scanning, device detection, capacity reporting
- **IDE / ATAPI driver** — Storage controller for CD-ROM and hard disks
- **Block device layer** — Unified interface for storage devices; partition registration and MBR parsing
- **RTC driver** — CMOS real-time clock with UIE interrupt
- **TCP/IP networking stack** — IPv4, ARP, ICMP, TCP, UDP, DHCP, DNS, NTP; Intel e1000 and VirtIO-Net drivers
- **EFI + BIOS hybrid ISO** — boots on OVMF UEFI and legacy BIOS via GRUB2

---

## Repository Layout

```
uaos/
├── kernel/
│   ├── boot/           # NASM entry point, C kernel main, linker script
│   ├── display/        # Framebuffer, desktop, cursor, window manager, shell window
│   ├── irq/            # IDT, 8259A PIC, PS/2 mouse, PS/2 keyboard, VMware mouse, RTC, VirtIO block
│   ├── exec/           # Thunk handler, MMU sandbox, page fault ISR, ROM modules, task scheduler,
│   │                   # syscall dispatch, native x86-64 ELF64 loader
│   ├── dos/            # VFS layer, RAM filesystem, block device layer, FAT32/PFS3/EXT4/ISO9660
│   ├── net/            # TCP/IP stack (IPv4, ARP, ICMP, TCP, UDP, DHCP, DNS, NTP)
│   ├── drivers/        # Network and storage drivers (e1000, virtio-net, virtio-blk, IDE)
│   └── shell/          # Native C: command implementations (cmd_*.c) and resident command system
├── emulation/
│   ├── binaries/       # Embedded M68k binaries (auto-wrapped into the kernel image)
│   ├── rom_patches/    # M68k Vasm/Devpac stubs, kickstart config
│   ├── src/musashi/    # M68k CPU emulator
│   ├── uaos_m68k_glue.c # M68k emulator glue, LVO stubs, DOS stubs
│   ├── uaos_uae_bridge.c  # UAE bridge and RAM-base management
│   └── uaos_emu_registry.c
├── system/             # Amiga-style filesystem skeleton (C, S, LIBS, L, DEVS, SYS, Tools)
│   ├── libuaos/        # Userspace C library headers and startup code
│   ├── userspace/      # Native x86-64 Ring-3 ELF64 programs (pwd, file, strings, find, Guide, ...)
│   └── S/              # Startup-Sequence, network, NTP, timezone configs
├── scripts/
│   ├── build_iso.sh    # Full build pipeline
│   └── grub.cfg        # GRUB2 multiboot2 configuration
├── tools/              # Host-side build helpers (gen_uaos_native, gen_uaos_m68k, gen_uaos_x64)
├── documentation/
│   ├── uaos.guide      # AmigaGuide database
│   ├── manual.md       # Markdown technical reference
│   ├── manual.tex      # LaTeX technical reference
│   └── Dos_Manual.md   # Shell and scripting reference
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
| 1 | Creates `build/` staging directories and the dynamic `SYS_ROOT` image |
| 2 | Builds host tools (`gen_uaos_native`, `gen_uaos_m68k`, `gen_uaos_x64`, `gen_m68k_library`) |
| 3 | Assembles `uaos_kernel_entry.asm`, `idt_stubs.asm` and `task_switch.asm` with NASM |
| 4 | Generates the Musashi M68k opcode table if needed |
| 5 | Compiles all C kernel sources with GCC (`-ffreestanding -m64 -O2 -std=c11`) |
| 6 | Links everything into `uaos-kernel.elf` (ELF64) via the custom linker script |
| 7 | Wraps embedded M68k binaries from `emulation/binaries/` and Amiga `.library` files |
| 8 | Builds native x86-64 Ring-3 userspace programs from `system/userspace/` |
| 9 | Stages the `system/` Amiga filesystem skeleton into `SYS_ROOT` (C:, S:, LIBS:, DEVS:, L:, SYS, Tools) |
| 10 | Injects `grub.cfg` and the AROS kickstart configuration |
| 11 | Produces a hybrid BIOS+EFI ISO with `grub-mkrescue` |

---

## Native x86-64 Userspace Programs

Programs in `system/userspace/` are compiled as position-independent x86-64
ELF64 binaries, linked against `system/libuaos/uaos_start.c`, and wrapped with a
32-byte `UAOS` header (`UAOS_BIN_TYPE_X64`). At runtime the kernel loads them
into Ring-3 tasks and enters user mode via `iretq`. They communicate with the
kernel through the `INT 0x80` syscall ABI:

```c
RAX = syscall number        RDI = arg 1   RSI = arg 2   RDX = arg 3
```

Current userspace tools:

| Program | Source | Syscalls used |
|---------|--------|---------------|
| `hello` | `system/userspace/hello.c` | `write` |
| `pwd` | `system/userspace/pwd.c` | `getcwd`, `write` |
| `file` | `system/userspace/file.c` | `open`, `read_file`, `stat`, `write`, `close` |
| `strings` | `system/userspace/strings.c` | `open`, `read_file`, `write`, `close` |
| `find` | `system/userspace/find.c` | `getcwd`, `opendir`, `readdir`, `closedir`, `write` |
| `Guide` | `system/userspace/guide.c` | GUI syscalls (`create_window`, `draw_text`, `present`, `get_event`) |

The syscall numbers are defined in `kernel/exec/syscall_table.h` (kernel) and
`system/libuaos/uaos_syscall.h` (userspace).

---

## Running in QEMU

### First-time setup — copy OVMF variables file

OVMF requires a writable variables file. **Copy a fresh copy before each run** —
stale vars can save a changed boot order and cause the firmware to drop to the
UEFI shell instead of booting from CD:

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
  -device piix3-ide,id=ide \
  -drive if=none,id=cdrom,media=cdrom,file=build/Ultimate_Amiga_OS.iso \
  -device ide-cd,drive=cdrom,bus=ide.0 \
  -m 512M \
  -vga virtio \
  -no-reboot \
  -no-shutdown
```

| Flag | Reason |
|------|--------|
| `-machine q35,usb=off` | Q35 chipset; `usb=off` disables USB tablet which conflicts with PS/2 mouse |
| `-drive if=pflash ...OVMF_CODE` | UEFI firmware (read-only) |
| `-drive if=pflash ...ovmf_vars` | UEFI variable store (writable copy) |
| `-device piix3-ide` | Explicit IDE controller (Q35 lacks built-in IDE; needed for ATAPI CD-ROM detect) |
| `-device ide-cd` | Attach CD-ROM to the IDE controller |
| `-vga virtio` | Best framebuffer performance under QEMU |
| `-no-reboot` | Keeps QEMU open if the kernel calls reboot (useful for debugging) |
| `-no-shutdown` | Keeps QEMU window open when guest CPU is idle (prevents window disappearing) |

### Optional: serial debug output

Add `-serial stdio` to see serial debug output from the kernel on your terminal:

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
  -no-shutdown \
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
| `dir [path]` | List files in current directory |
| `cd [path]` | Change or show current directory |
| `makedir <path>` | Create a directory |
| `delete <path>` | Delete a file or empty directory |
| `type <file>` | Display file contents |
| `copy <src> <dst>` | Copy a file |
| `rename <from> <to>` | Rename or move a file |
| `pwd` | Print working directory (userspace Ring-3 utility) |
| `echo <text>` | Print text to shell |
| `pointer` | Open pointer preferences |
| `protect <flags> <path>` | Set file attributes (`+r`, `-r`, `+h`, `-h`) |
| `attr <path>` | Show file attributes (Read-Only, Hidden, etc.) |
| `info [device]` | Show mounted disks and volumes; or info for a specific device |
| `alias [name cmd]` | Create or list command aliases (built-in) |
| `unalias <name>` | Remove an alias (built-in) |
| `set [name val]` | Set or list local variables (built-in) |
| `unset <name>` | Remove a local variable (built-in) |
| `path [dirs...]` | Show or set the command search path (built-in) |
| `setenv <name> <value>` | Set a global environment variable (built-in) |
| `unsetenv <name>` | Remove a global environment variable (built-in) |
| `showconfig` | Show hardware configuration (built-in) |
| `date` | Show current date and time |
| `which <cmd>` | Locate a command |
| `disks` | List detected block devices |
| `fdisk <device>` | Partition a block device |
| `format <dev> [fs]` | Format a partition (FAT32) |
| `run <cmd> [args]` | Run a command in a new CLI |
| `ifconfig [dhcp \| <ip> <gw>]` | Configure or show network settings |
| `ping <host> [count]` | Send ICMP echo requests |
| `route` | Show routing table and ARP cache |
| `nslookup <host> [server]` | Resolve a hostname via DNS |
| `ntpd [server]` | Synchronise time via NTP |
| `netstart` / `netstop` | Start or stop the network stack |
| `netinfo` | Open the network information window |
| `grep [-i] <pattern> <file>` | Search a file for a pattern |
| `more <file>` | Paginated file viewer |
| `file <path>...` | Identify file format from magic numbers |
| `strings <path>... [-n minlen]` | Extract printable strings |
| `find [path] [-name pat] [-type f\|d]` | Recursively search directories |
| `list` | List files with detailed information |
| `search <pattern> [file]` | Advanced file search |
| `sort [file] [options]` | Sort file lines |
| `join <file1> <file2>` | Join two files by key |
| `ps` | List running tasks |
| `jobs` | List background jobs |
| `wait` | Wait for background jobs |
| `changetaskpri <pri> [task]` | Change task priority |
| `ask <prompt>` | Prompt the user for input |
| `calculator` | Open the calculator window |
| `clock` | Open the clock window |
| `loadwb` | Launch the Workbench desktop |
| `vim <file>` | Open the inline text editor |
| `newcli` / `newshell` | Open a new shell window |
| `execute <script>` | Execute a script file |
| `assign [name: target]` | Create or list assigns |
| `getenv <name>` | Read an environment variable |
| `resident` | Manage resident commands |
| `status` | Show system status |
| `avail` | Show available memory |
| `filenote <file> <comment>` | Set a file comment |
| `relabel <device> <name>` | Rename a volume |
| `install <device>` | Install a boot block |
| `diskchange <device>` | Notify the system of a disk change |
| `addbuffers <device> <n>` | Add disk buffers |
| `requestchoice <title> <body> <buttons...>` | Show a choice dialog |
| `requestfile [options]` | Show a file requester dialog |
| `prompt <string>` | Set a custom shell prompt |
| `stack` | Show stack usage |
| `why` | Show the last command return code |
| `failat <n>` | Set the failure threshold |
| `quit [rc]` | Exit a script |
| `endcli` | Close the current shell window |

---

## Architecture Overview

```
GRUB2 Multiboot2
    └── uaos_kernel_entry.asm   (32-bit protected → 64-bit long mode)
            └── uaos_kernel_main.c
                    ├── FB_Init()           framebuffer from Multiboot2 tag
                    ├── IDT_Init()          256-vector IDT + 8259A PIC remap
                    ├── APIC_Init()         Local APIC configuration
                    ├── PS2Mouse_Init()     IRQ12 PS/2 mouse driver
                    ├── PS2Kbd_Init()       IRQ1  PS/2 keyboard driver
                    ├── RTC_Init()          CMOS real-time clock (IRQ8)
                    ├── VFS_Init()          VFS layer + RAM filesystem
                    ├── BlockDev_Init()     Block device layer
                    ├── virtio_blk_init()   VirtIO block device driver
                    ├── ide_init()          IDE/ATAPI controller
                    ├── UAOS_MMU_Init()     MMU sandbox page tables
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
                    │   ├── dos.library v40 (→ VFS)
                    │   ├── bsdsocket.library v4 (→ TCP/IP stack)
                    │   ├── workbench.library v45
                    │   └── intuition.library v40
                    ├── net_stack_init()    TCP/IP stack + NIC auto-probe
                    ├── Task_Init()         Ring-3 task scheduler / TSS
                    ├── Desktop_Draw()      Workbench backdrop + icons
                    ├── ShellWin_Init()     Shell window → registers with WM
                    └── event loop
                            ├── WM_MouseEvent()   drag / focus / resize
                            ├── WM_KeyEvent()     routes keystrokes to focused window
                            ├── net_stack_poll()  process RX frames
                            └── Syscall_Dispatch()  INT 0x80 from Ring-3 tasks
```

---

## Known Limitations

- M68k emulation has basic LVO stubs but needs full memory access integration
- ROM library functions are stubs with implementation logic in comments
- VirtIO block device read/write needs virtqueue I/O implementation
- FAT32 partition read/write via virtqueue I/O not yet implemented
- No networking, no audio
- Single CPU, no SMP
- Clock display updates via RTC but needs full date/time integration

---
<img width="1413" height="1069" alt="image" src="https://github.com/user-attachments/assets/66433a49-276c-4408-a7e7-6b869220c57e" />

## License

This project is a personal research and hobby project. No licence is currently applied.
