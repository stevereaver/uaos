---
type: Infrastructure
title: UAOS Build System
description: Details of the toolchain and process used to build UAOS.
resource: /scripts/
tags: [build, gcc, nasm, grub, iso]
timestamp: 2026-06-18T10:00:00Z
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

1. **Staging**: Creates a `build/` directory for object files and ISO root.
2. **Assembly**: Assembles `.asm` files (e.g., `uaos_kernel_entry.asm`).
3. **Compilation**: Compiles all kernel C files.
4. **Linking**: Links objects into `uaos-kernel.elf`.
5. **System Root**: Packages the `system/` directory (Amiga-style layout).
6. **GRUB Config**: Injects `scripts/grub.cfg`.
7. **ISO Generation**: Runs `grub-mkrescue` to create the final `Ultimate_Amiga_OS.iso`.

## Hybrid Boot

UAOS supports both legacy BIOS and UEFI via a hybrid ISO:
- **BIOS**: Uses GRUB's `i386-pc` core.
- **UEFI**: Includes a standalone GRUB EFI image (`bootx64.efi`).
