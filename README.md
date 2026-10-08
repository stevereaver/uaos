# Ultimate Amiga OS (UAOS)

A bare-metal x86_64 hobby operating system inspired by the Amiga Workbench 3.x aesthetic, built from scratch using NASM, C11, GRUB2, and OVMF/UEFI.

UAOS boots directly from a hybrid ISO via GRUB2 Multiboot2, initialises a linear framebuffer, and presents a graphical Workbench-style desktop with a window manager, an interactive shell, and a hybrid execution model: a native x86_64 kernel that can also run classic Amiga M68k binaries through Musashi CPU emulation with "thunking" into AmigaOS-compatible native libraries.

---

## Features

### Desktop & GUI

- **Workbench-style desktop** — solid Amiga grey backdrop, menu bar with live CPU% indicator, status bar, disk icons with multi-select, drag, and double-click launch (WBStartup messages for M68k tools)
- **Window manager** — multiple windows, click-to-focus, z-order, title-bar drag, resize grip, gadgets, requesters, and BOOPSI gadgets
- **Built-in applications** — file browser, `ed`/`vim` text editors, AmigaGuide help viewer (`guide`), calculator, clock, preferences editors (`prefs`, `pointer`), Exchange commodity manager, format wizard, and a screen blanker
- **Screenshot capture** — `screenshot` writes the framebuffer to JPEG

### Input

- **PS/2 mouse and keyboard** — IRQ12/IRQ1-driven, scancode set 1, i8042 self-test gating
- **USB input** — UHCI host controller, USB hub class driver, USB HID, Apple Geyser III/IV (`appletouch`) and BCM5974 trackpads, Apple Fn-chord input
- **VMware mouse** — absolute-pointer fallback when available
- **Ctrl+LAmiga+RAmiga** — three-finger reset chord

### Shell & Commands

- **Shell window** — scrollable history anchored on resize, input line, per-shell `S:Shell-Startup` script, built-ins (`help`, `cd`, `alias`, `set`, `path`, `setenv`, `prompt`, ...)
- **110+ native C: commands** — AmigaDOS-compatible file, volume, script, network, desktop, and diagnostics commands (see the command tables below)
- **Resident commands** — `resident`/`resload` keep commands in memory
- **ARexx** — `rx` runs scripts via the bundled Regina Rexx interpreter (`REXX:`)
- **ACEBasic** — the ACE BASIC interpreter is staged onto the ISO (`ACE:`)

### Execution & Emulation

- **Ring-3 userspace programs** — native x86-64 ELF64 binaries entered via `iretq`, using the `INT 0x80` syscall ABI
- **GNU coreutils** — ~90 utilities under the `gnu:` assign, including `wget`/`curl` with HTTPS via BearSSL
- **M68k emulation** — Musashi CPU core, Amiga Hunk binary loader, ILLEGAL-opcode LVO dispatch, wild-PC circuit breaker; runs raw Amiga binaries and embedded M68k demos
- **Custom-chip emulation** — AGA/ECS chipset register window serviced by the page-fault handler, with `chiptrace` access tracing; floppy chipset emulation
- **ROM module system** — native AmigaOS-compatible libraries registered in a unified dispatch registry, plus loadable `.library` files scanned from `LIBS:`:
  - `exec.library` v45 — processes, memory, signals, IPC
  - `dos.library` v40 — file system operations, WBStartup
  - `intuition.library` v40 — GUI API, asl.library-style file requester
  - `graphics.library` v40 — graphics primitives
  - `gadtools.library`, `iffparse.library` v39, `icon.library`, `workbench.library` v45
  - `utility.library` v37, `locale.library` v38, `ixemul.library` v53
  - `mathffp.library` v40, `mathieeesingbas.library`, `mathtrans.library`
  - `bsdsocket.library` v4 — BSD socket API over the native TCP/IP stack
  - `console.device` v40, `timer.device` v40, `keyboard.device` v40, `audio.device` stub

### Storage & Filesystems

- **Storage drivers** — VirtIO block, VirtIO SCSI, IDE/ATAPI, AHCI SATA, floppy; MBR partition parsing and registration
- **Filesystems** — FAT32 (with VFAT long filenames), FFS, PFS3, EXT4 (read-only), ISO9660 (CD-ROM), RAMFS, and CrossDOS (FAT12/16) media
- **Packet-handler architecture** — asynchronous MsgPort-based handlers; built-in FAT/FFS/CrossDOS/AUX/PORT/PRINT handlers plus loadable handlers scanned from `L:`
- **VFS / RAM filesystem** — in-memory node tree auto-mounted at boot with T, ENV, CLIPS, S dirs; `fsck` integrity checker

### Hardware & Platform

- **IDT / interrupt controllers** — 256-vector IDT, 8259A PIC remap, local APIC + IOAPIC, ACPI table parsing
- **MMU sandbox** — 4-level paging, 2 MB huge pages, MTRR management
- **USB stack** — UHCI HCD with EHCI port handoff, hub enumeration, deaf-port heartbeat re-probe
- **Networking** — IPv4, ARP, ICMP, TCP (multi-segment retransmit queue), UDP, DHCP, DNS, NTP with timezone support; Intel e1000, VirtIO-Net, and Marvell sky2 drivers; `telnetd` remote shell daemon
- **Audio** — PC speaker and AC97 output paths
- **Platform drivers** — EIST CPU frequency scaling (`cpu`), NVIDIA SOR-PWM backlight (`backlight`), entropy pool, RTC/CMOS clock

### Debugging & Observability

- **klog** — unified kernel logging with per-subsystem masks and a 48 KB ring buffer (`klog`, `dmesg`)
- **Serial console** — `sercon` serves a command task on COM1; UART is the canonical log sink
- **In-guest diagnostics** — `strace`, `etrace`, `prof`, `memcheck`, `failalloc`, `watchdog`, `tickcheck`, `irqstat`/`irqaudit`/`irqroute`, `pciscan`, `usbdiag`, `diskdiag`, `netstat`, `pktmon` (pcap), `ports`, `timers`, `handles`, `taskdump`/`taskstat`, `peek`/`poke`, `crash`
- **Host-side tooling** — QEMU GDB stub (`scripts/debug_qemu.sh`), `tools/symbolize.sh`, `analyze_log.py`, `etrace_decode.py`, `prof_report.py`, `gdb_uaos.py`
- **EFI + BIOS hybrid ISO** — boots on OVMF UEFI and legacy BIOS via GRUB2

---

## Repository Layout

```
uaos/
├── kernel/
│   ├── boot/           # NASM entry point, C kernel main, linker script
│   ├── display/        # Framebuffer, desktop, window manager, shell window,
│   │                   # file browser, editors, prefs windows, blanker,
│   │                   # icon renderer, JPEG encoder, UI toolkit (uitree)
│   ├── irq/            # IDT, 8259A PIC, APIC/IOAPIC, ACPI, PS/2, VMware
│   │                   # mouse, RTC, VirtIO block/SCSI
│   ├── exec/           # MMU sandbox, task scheduler, syscall dispatch,
│   │                   # ELF64 loader, ROM module registry, native AmigaOS
│   │                   # libraries, BOOPSI, loadable LIBS: libraries
│   ├── dos/            # VFS, RAMFS, FAT32/VFAT, FFS, PFS3, EXT4, ISO9660,
│   │                   # packet handlers, handler loader, partitions, fsck
│   ├── net/            # TCP/IP stack (IPv4/ARP/ICMP/TCP/UDP/DHCP/DNS/NTP),
│   │                   # telnetd, pktmon, timezone
│   ├── drivers/        # AHCI, IDE, floppy, e1000, virtio-net, sky2,
│   │                   # UHCI + USB core + hub + HID, appletouch/bcm5974,
│   │                   # cpufreq (EIST), nv50bl backlight, entropy
│   ├── audio/          # PC speaker, AC97, audio.device
│   ├── chipset/        # AGA/ECS custom-chip emulation, chiptrace, floppy
│   ├── klog/           # Unified kernel log, UART, serial console
│   ├── dbg/            # etrace, prof, failalloc, watchdog, tickmon, sysinfo
│   └── shell/          # 110+ native C: commands (cmd_*.c), resident system
├── emulation/
│   ├── binaries/       # Embedded M68k binaries (wrapped into the image)
│   ├── rom_patches/    # M68k Vasm/Devpac stubs, kickstart config
│   ├── musashi/        # M68k CPU emulator (git submodule)
│   ├── uaos_m68k_glue.c # M68k emulator glue, LVO stubs, DOS stubs
│   ├── uaos_uae_bridge.c  # UAE bridge and RAM-base management
│   └── uaos_emu_registry.c
├── system/             # Amiga-style filesystem skeleton (C, S, LIBS, L,
│   │                   # DEVS, SYS, Tools, Prefs, Demos, REXX, ACE, gnu)
│   ├── libuaos/        # Userspace C library: startup, syscall, socket,
│   │                   # TLS (uaos_tls.h), HTTP, GUI, getopt, hash headers
│   ├── userspace/      # Native x86-64 Ring-3 ELF64 programs
│   ├── gnusrc/         # GNU coreutils sources (~90 utilities + wget/curl)
│   ├── bearssl/        # BearSSL subset — TLS client for gnu wget/curl
│   ├── Demos/          # M68k demo sources (assembled by vasm at build time)
│   ├── gnu/            # POSIX directory skeleton exposed via the gnu: assign
│   └── S/              # Startup-Sequence, Shell-Startup, User-Startup,
│                       # net/NTP/timezone configs
├── scripts/            # build_iso.sh (compat wrapper), grub.cfg, QEMU
│                       # launchers, debug_qemu.sh, net_bridge_setup.sh
├── tests/              # Host-side harnesses: smoke.sh, qemu_layout_test.py,
│                       # qemu_wblaunch_test.py, qemu_asl_test.py,
│                       # qemu_m68k_lifecycle_test.py, qemu_octamed_iff_test.py,
│                       # disk-image fixtures
├── tools/              # Host-side generators (gen_uaos_*, gen_m68k_library)
│                       # and debug tools (symbolize.sh, analyze_log.py,
│                       # etrace_decode.py, prof_report.py, gdb_uaos.py)
├── assets/             # splash.jpg (GRUB menu + kernel splash)
├── okf/                # OKF knowledge bundle — subsystem docs and log
├── documentation/      # uaos.guide, manual.md/.tex, Dos_Manual.md
└── build/              # Generated output
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
    qemu-system-x86 \
    wget \
    lhasa
```

`wget` and an `lha`-compatible extractor are used to fetch third-party
build inputs (vasm/vlink, Regina Rexx, ACEBasic) on first build; downloads
are cached under `build/` afterwards.

---

## Building the ISO

From the repository root (GNU Make; parallel and incremental):

```bash
make -j$(nproc)
```

Other targets (`make help`):

| Target | Action |
|--------|--------|
| `make iso` | Build `build/Ultimate_Amiga_OS.iso` (default) |
| `make kernel` | Build `build/uaos-kernel.elf` only |
| `make sysroot` | Build the SYS_ROOT module image only |
| `make tools` | Host-side generator tools only |
| `make check` | Build + run the uitree layout self-test |
| `make clean` | Remove `build/` |
| `make distclean` | Clean + generated in-tree files |

(`scripts/build_iso.sh` remains as a compatibility wrapper.)

On success the ISO is written to:

```
build/Ultimate_Amiga_OS.iso
```

### What the build does

| Step | Action |
|------|--------|
| 1 | Creates `build/` staging directories and the dynamic `SYS_ROOT` image |
| 2 | Builds host tools (`gen_uaos_native`, `gen_uaos_m68k`, `gen_uaos_x64`, `gen_m68k_library`) |
| 3 | Fetches third-party inputs on demand: vasm/vlink, Regina Rexx, ACEBasic (cached in `build/`) |
| 4 | Assembles `uaos_kernel_entry.asm`, `idt_stubs.asm` and `task_switch.asm` with NASM |
| 5 | Generates the Musashi M68k opcode table if needed |
| 6 | Compiles all C kernel sources with GCC (`-ffreestanding -m64 -O2 -std=c11`) |
| 7 | Builds the BearSSL static library used by gnu `wget`/`curl` |
| 8 | Links everything into `uaos-kernel.elf` (ELF64) via the custom linker script |
| 9 | Wraps embedded M68k binaries; assembles `system/Demos/src/*.s` with vasm |
| 10 | Builds native x86-64 Ring-3 userspace programs and the GNU utilities |
| 11 | Stages the `system/` Amiga filesystem skeleton into `SYS_ROOT` (C:, S:, LIBS:, DEVS:, L:, SYS, Tools, Prefs, Demos, REXX, ACE, gnu) |
| 12 | Injects `grub.cfg` and the kickstart configuration |
| 13 | Produces a hybrid BIOS+EFI ISO with `grub-mkrescue` |

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

Current userspace tools (`system/userspace/`):

| Program | Description |
|---------|-------------|
| `hello` | Minimal syscall smoke test |
| `pwd` | Print working directory |
| `dir`, `list` | Directory listings |
| `type`, `more` | File viewers |
| `copy`, `rename`, `makedir`, `delete` | File operations |
| `protect`, `attr`, `filenote` | File metadata |
| `echo` | Print text |
| `file` | Identify file format from magic numbers |
| `strings` | Extract printable strings |
| `find`, `search`, `grep` | Search files and trees |
| `sort`, `join` | Text processing |
| `avail` | Available memory |
| `memtest` | Memory tester |
| `Guide` | GUI AmigaGuide viewer (GUI syscalls) |
| `uidemo` | UI toolkit demo |

The syscall numbers are defined in `kernel/exec/syscall_table.h` (kernel) and
`system/libuaos/uaos_syscall.h` (userspace).

---

## GNU Core Utilities (`gnu:` layer)

UAOS ships ~90 GNU-style utilities alongside the AmigaDOS-style commands.
The GNU tools use GNU-style flags (`--long`, `-s`, `-n 5`) parsed by
`system/libuaos/uaos_getopt.h`, a freestanding `getopt_long` implementation.
The AmigaDOS commands in `C:` (e.g. `sort`, `join`) are kept unchanged; the
GNU equivalents live under the `gnu:` assign, which `S:Startup-Sequence`
maps to `Workbench:gnu`.

### Directory layout

```
gnu:
├── bin/              # symlink-style bin directory
├── usr/
│   └── bin/          # GNU coreutils binaries (cat, wc, sort, md5sum, ...)
└── usr/local/bin/    # reserved for user-installed tools
```

### Available utilities

Sources live in `system/gnusrc/` and are compiled by the Makefile's
`gnusrc` rules (part of the default `iso` target):

| Category | Tools |
|----------|-------|
| Core text | `cat` `tac` `nl` `wc` `head` `tail` `cut` `tr` `uniq` `fold` `expand` `unexpand` |
| Advanced text | `paste` `comm` `fmt` `sort` `seq` `tsort` `shuf` `split` `csplit` |
| Encoding | `base32` `base64` `basenc` `od` `uuencode` `uudecode` |
| Checksums | `sum` `cksum` `md5sum` `sha1sum` `sha256sum` `sha512sum` `b2sum` |
| Network | `wget` `curl` (HTTPS via BearSSL) |
| Other text | `pr` `numfmt` `ptx` |
| File listing/info | `ls` `dir` `vdir` `stat` `df` `du` `basename` `dirname` `realpath` `pathchk` `mktemp` |
| File manipulation | `cp` `mv` `rm` `mkdir` `rmdir` `install` `touch` `truncate` `shred` `unlink` `dd` |
| Shell basics | `echo` `printf` `yes` `true` `false` `test` `expr` `factor` `sleep` `tee` `date` `env` `printenv` |
| System info | `uname` `arch` `nproc` `hostname` `hostid` `tty` `whoami` `logname` `id` `groups` `who` `users` `pinky` |
| User/group | `chmod` `chown` `chgrp` |

The checksum tools use `system/libuaos/uaos_hash.h`, a freestanding
implementation of MD5, SHA-1, SHA-256, SHA-512, BLAKE2b, and CRC32.
`wget`/`curl` link against the in-tree BearSSL subset and the
`uaos_tls.h`/`uaos_http.h` client helpers.

`chmod` maps POSIX octal/symbolic permission modes to AmigaDOS `FIBF_*`
protection bits.  `chown` and `chgrp` accept arguments but are no-ops on
the single-user UAOS system.

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

Add `-serial stdio` to see kernel serial output (the `klog` ring buffer is
mirrored to COM1) on your terminal:

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

With `-serial stdio` you can also type `sercon ON` inside the guest to get a
bidirectional command console on COM1.

### Kernel debugging with GDB

`scripts/debug_qemu.sh` boots the ISO under QEMU's built-in GDB stub
(`-gdb tcp::1234 -S`): the CPU starts halted so you can set breakpoints
before the first kernel instruction runs.

```bash
make -j$(nproc)                   # build the ISO first
./scripts/debug_qemu.sh           # QEMU waits on :1234

gdb build/uaos-kernel.elf
(gdb) target remote :1234
(gdb) hbreak uaos_kernel_main     # hardware breakpoint — see notes
(gdb) continue
```

Notes:

- The kernel runs in long mode on a 4 GB identity map, so ELF symbol
  addresses match guest addresses directly.
- Prefer `hbreak` over `break` — software breakpoints need write access
  to the code page.
- Serial debug output still goes to `/tmp/uaos_serial.log` (override
  with `SERIAL_LOG=`); the stub port can be changed with `GDB_PORT=`.
- `tools/gdb_uaos.py` adds UAOS-aware GDB helpers (task lists, symbol
  resolution); `tools/symbolize.sh` resolves raw addresses to symbols.

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
| Double-click disk/tool icon | Open volume window or launch the tool |
| Shift-click icons | Multi-select |

> QEMU captures the mouse when you click inside the window. Press **Ctrl+Alt+G** to release it.

### Shell window

Click the **UAOS Shell** title bar to focus it, then type commands.
Highlights by category (`help` lists everything in the guest):

#### Files & volumes

| Command | Description |
|---------|-------------|
| `dir [path]` / `list` | List files (simple / detailed) |
| `cd [path]` / `pwd` | Change or print working directory |
| `makedir <path>` | Create a directory |
| `type <file>` / `more <file>` | Display file contents |
| `copy <src> <dst>` / `rename <from> <to>` | Copy / move files |
| `delete <path>` | Delete a file or empty directory |
| `protect`, `attr`, `filenote` | Protection bits, attributes, comments |
| `info`, `disks`, `diskchange`, `addbuffers` | Volume/device info and control |
| `mount`, `crossdos` | Mount handlers / PC-format FAT12/16 media |
| `fdisk`, `format`, `fsck`, `install`, `relabel` | Partition, format, check, bootblock, rename volume |
| `which`, `search`, `grep`, `sort`, `join` | Locate and process files |
| `file`, `strings`, `find` | (Ring-3 userspace tools) |

#### Shell, scripts & environment

| Command | Description |
|---------|-------------|
| `alias`/`unalias`, `set`/`unset`, `setenv`/`unsetenv`/`getenv` | Aliases and variables |
| `path`, `prompt` | Search path and prompt |
| `execute`, `run`, `runback`, `newcli`, `endcli` | Scripts, background jobs, new shells |
| `resident`, `resload` | Resident command list |
| `rx <program>` | Run an ARexx script via Regina Rexx |
| `ask`, `failat`, `why`, `quit`, `skip`, `lab` | Script flow control |
| `echo`, `date`, `version`, `status`, `stack` | Basics |

#### System & tasks

| Command | Description |
|---------|-------------|
| `ps`, `jobs`, `wait`, `changetaskpri` | Task and job control |
| `mem`, `avail`, `libs`, `showconfig` | Memory, libraries, hardware inventory |
| `cpu` | CPU frequency/power introspection |
| `reboot` | Reboot (also Ctrl+LAmiga+RAmiga) |
| `clear` | Clear shell history |

#### Desktop & GUI

| Command | Description |
|---------|-------------|
| `loadwb`, `wbrun` | Launch Workbench / a tool with WBStartup semantics |
| `ed`, `vim`, `guide` | Editors and the AmigaGuide viewer |
| `calculator`, `clock` | Applets |
| `prefs`, `pointer`, `blanker`, `backlight` | Preferences, pointer, blanker, panel brightness |
| `exchange` | Commodity exchange manager |
| `requestchoice`, `requestfile` | Dialog requesters |
| `screenshot` | Capture the screen to JPEG |
| `netinfo` | Network information window |

#### Networking

| Command | Description |
|---------|-------------|
| `ifconfig`, `route`, `netstart`/`netstop` | Interface and stack control |
| `ping`, `nslookup`, `ntpd` | Connectivity, DNS, time sync |
| `netstat` | Live TCP/UDP socket table |
| `telnetd` | Start/stop the remote shell daemon |
| `pktmon` | In-guest pcap capture |

#### Diagnostics & debugging

| Command | Description |
|---------|-------------|
| `dmesg`, `klog` | Dump/filter the kernel log; per-subsystem log masks |
| `sercon` | Serial command console on COM1 |
| `strace`, `etrace`, `prof` | Syscall, event, and RIP-sampling profilers |
| `memcheck`, `failalloc` | Heap debugging and alloc-failure injection |
| `watchdog`, `tickcheck` | Stall watchdog, PIT/IRQ latency check |
| `irqstat`, `irqaudit`, `irqroute` | IRQ counters, hold-time audit, routing |
| `pciscan`, `usbdiag`, `diskdiag` | Bus/storage register dumps |
| `ports`, `timers`, `handles` | MsgPort, timer.device, and handle dumps |
| `taskdump`, `taskstat` | Task frames and CPU accounting |
| `peek`, `poke` | Physical/MMIO read and write |
| `chiptrace` | Custom-chip/CIA access tracer |
| `print` | Send a file to PRT: |
| `crash` | Deliberate fault for panic-path testing |

---

## Architecture Overview

For a full interactive diagram see **[ARCHITECTURE.md](ARCHITECTURE.md)**.

```
GRUB2 Multiboot2
    └── uaos_kernel_entry.asm   (32-bit protected → 64-bit long mode)
            └── uaos_kernel_main.c
                    ├── uart_init()           16550A serial + klog ring
                    ├── Dbgcon_Init()         debug console backend
                    ├── FB_Init()             framebuffer from Multiboot2 tag
                    ├── UAOS_MMU_Init()       4-level paging sandbox + MTRR
                    ├── entropy_init()        entropy pool
                    ├── audio_init()          PC speaker / AC97
                    ├── UAOS_Bridge_Init()    M68k UAE bridge (Musashi)
                    ├── VFS_Init()            VFS + RAM filesystem
                    ├── BlockDev_Init()       block device layer
                    │   ├── FloppyBlockDev_Init()
                    │   ├── virtio_blk_init() / virtio_scsi_init()
                    │   ├── IDE_Init()        IDE/ATAPI
                    │   └── AHCI_Init()       SATA (polled, pre-IDT)
                    ├── USB stack             UHCI → hub → HID /
                    │                       BCM5974 / AppleTouch class drivers
                    ├── DosList_Init()        device/volume/assign list
                    ├── HandlerLoader_Init()  packet handlers + L: scan
                    ├── UAOS_LoadableLib_Init()  loadable LIBS: libraries
                    ├── IDT_Init()            256-vector IDT
                    │                         (#PF → chipset register window)
                    ├── PIC/APIC/IOAPIC/ACPI  interrupt controllers
                    ├── SysInfo_Init()        live hardware inventory
                    ├── PS2Mouse/PS2Kbd_Init()  (i8042 self-test gated)
                    ├── RTC_Init()            CMOS real-time clock (IRQ8)
                    ├── VMMouse_Init()        VMware mouse fallback
                    ├── CpuFreq_Init()        EIST frequency scaling
                    ├── NV50BL_Init()         NVIDIA SOR-PWM backlight
                    ├── TaskScheduler_Init()  Ring-3 task scheduler / TSS
                    ├── UAOS_ROM_RegisterAll() ROM module registry
                    │   ├── exec.library v45        ├── dos.library v40
                    │   ├── intuition.library v40   ├── graphics.library v40
                    │   ├── gadtools.library        ├── iffparse.library v39
                    │   ├── icon.library            ├── workbench.library v45
                    │   ├── utility.library v37     ├── locale.library v38
                    │   ├── ixemul.library v53      ├── mathffp/mathtrans/
                    │   │                             mathieeesingbas
                    │   ├── bsdsocket.library v4    ├── console.device v40
                    │   ├── timer.device v40        ├── keyboard.device v40
                    │   └── audio.device (stub)
                    ├── Sercon_Start()        serial command console
                    ├── net_stack_init()      TCP/IP + NIC auto-probe
                    ├── UserWindow_Init()     user-window registry
                    ├── Desktop_Draw()        Workbench backdrop + icons
                    ├── ShellWin_Init()       shell window → registers with WM
                    └── event loop
                            ├── WM_MouseEvent()   drag / focus / resize
                            ├── WM_KeyEvent()     routes keystrokes (incl. USB HID)
                            ├── net_stack_poll()  process RX frames + TCP retx
                            └── Syscall_Dispatch()  INT 0x80 from Ring-3 tasks
```

---

## Known Limitations

- **Audio is minimal** — PC speaker and AC97 output exist, but `audio.device` is only a stub and there is no full playback path yet.
- **M68k emulation is partial** — Musashi, the Hunk loader, custom-chip register emulation, and thunking can run real Amiga binaries and the bundled demos; full custom-chip behaviour and complex AmigaOS software compatibility are still being completed.
- **ROM library coverage is incomplete** — the native AmigaOS-compatible libraries are functional implementations, but not every AmigaOS API is available.
- **Storage/filesystem coverage varies** — VirtIO, IDE, AHCI, floppy, FAT32, FFS, PFS3, ISO9660 and RAMFS are exercised; EXT4 is read-only and PFS3 support is partial.
- **Single CPU only** — no SMP/multicore support.
- **USB is UHCI-only** — USB 1.1 devices work; there is no EHCI/xHCI host driver (EHCI-owned ports are handed off to UHCI companions where possible).

---
<img width="1230" height="922" alt="image" src="https://github.com/user-attachments/assets/cd9f836a-e78a-40dc-993d-5f59b2e8fad1" />

## License

UAOS-original code is released under the [MIT License](LICENSE).

The UAOS kernel and system files are MIT-licensed.  The bootable ISO
image also contains [GNU GRUB](https://www.gnu.org/software/grub/)
(GPL-3.0-or-later) as the bootloader — GRUB is a separate work loaded
via the multiboot2 protocol and is not linked into the UAOS kernel.

Third-party source code bundled in this repository (Musashi M68k
emulator, BearSSL, SoftFloat 2b, M68k PMMU) is licensed under their own
terms.  SoftFloat 2b and the M68k PMMU file are retained in the source
tree for reference but are **not compiled into the kernel**.  The build
also downloads third-party binaries (vasm/vlink, Regina Rexx, ACEBasic)
which remain under their own licenses.  See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for full details
including build tool licenses.
