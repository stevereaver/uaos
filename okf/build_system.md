---
type: Infrastructure
title: UAOS Build System
description: Details of the toolchain and process used to build UAOS.
resource: /scripts/
tags: [build, gcc, nasm, grub, iso]
timestamp: 2026-06-24T17:00:00Z
---

# UAOS Build System

UAOS uses a custom build pipeline to produce a hybrid BIOS/UEFI bootable ISO image.

## Toolchain

- **Compiler**: `gcc` (targetting `x86_64-elf` or host with `-ffreestanding`).
- **Assembler**: `nasm` for bootloader and interrupt stubs.
- **Linker**: `ld` with a custom linker script (`uaos_kernel.ld`).
- **ISO Creation**: `grub-mkrescue` and `xorriso`.

## Build Process

The primary build script is `scripts/build_iso.sh`.

1. **Staging**: Creates `build/` directories for object files and the ISO root.
2. **Host Tools**: Builds `tools/gen_uaos_native`, `tools/gen_uaos_m68k`, `tools/gen_uaos_x64`, and `tools/gen_m68k_library`.
3. **M68k Library Generation**: Generates loadable Amiga `.library` wrappers (e.g., `powerpacker.library`) in `system/LIBS/`.
4. **Assembly**: Assembles `.asm` files (`uaos_kernel_entry.asm`, `idt_stubs.asm`, `task_switch.asm`) with `nasm`.
5. **Musashi Generation**: Generates the Musashi M68k opcode table (`emulation/src/musashi/m68kops.c`) if it is missing.
6. **Binary Embedding**: Converts any files in `emulation/binaries/` to C byte arrays via `scripts/embed_binary.sh`.
7. **Compilation**: Compiles all kernel C files with `-ffreestanding -fno-stack-protector -fno-pie -fno-PIE -mno-red-zone -nostdlib -m64 -O2 -std=c11`.
8. **Linking**: Links objects into `uaos-kernel.elf` via `kernel/boot/uaos_kernel.ld`.
9. **Userspace Programs**: Compiles C utilities in `system/userspace/` with `-ffreestanding -nostdlib -fPIE -pie`, links them with `uaos_start.o`, wraps the resulting binaries using `gen_uaos_x64`, and packages them into `SYS_ROOT/C/`.
10. **System Root**: Packages the `system/` directory (Amiga-style `C:`, `S:`, `LIBS:`, `DEVS:`, `L:`, `SYS:`, `Tools:`) into `SYS_ROOT`.
11. **GRUB Config**: Injects `scripts/grub.cfg` and the AROS kickstart configuration.
12. **ISO Generation**: Runs `grub-mkrescue` to create the final `build/Ultimate_Amiga_OS.iso`.

## Helper Scripts

- `scripts/embed_binary.sh` — converts an Amiga Hunk binary into a C byte array for embedding.
- `scripts/create_disk.sh` — creates a QEMU `qcow2` disk image.
- `scripts/run_with_disk.sh` — launches UAOS in QEMU with optional disk and network modes.
- `scripts/net_bridge_setup.sh` — configures a host TAP/bridge network for QEMU.

## Hybrid Boot

UAOS supports both legacy BIOS and UEFI via a hybrid ISO:
- **BIOS**: Uses GRUB's `i386-pc` core.
- **UEFI**: Includes a standalone GRUB EFI image (`bootx64.efi`).
