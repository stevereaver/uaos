#!/bin/bash
# debug_qemu.sh — Boot UAOS under QEMU's built-in GDB stub.
#
# Launches QEMU with "-s -S": the CPU starts halted and a GDB stub
# listens on tcp::1234.  Attach with:
#
#   gdb build/uaos-kernel.elf
#   (gdb) target remote :1234
#   (gdb) hbreak uaos_kernel_main
#   (gdb) continue
#
# Environment variables:
#   GDB_PORT=1234   stub listen port
#   SERIAL_LOG=/tmp/uaos_serial.log   where COM1 debug output goes
#   ISO=build/Ultimate_Amiga_OS.iso   image to boot
#   NET=user        user-mode networking (virtio-net), like run_with_disk.sh
#
# Caveats:
#   * Kernel runs in 64-bit long mode on a 4 GB identity map — physical
#     and virtual addresses are the same, so symbols resolve directly.
#   * GRUB loads the kernel at its link address; breakpoints on ELF
#     symbols work once multiboot2 jumps into uaos_kernel_entry.
#   * Use hbreak (hardware breakpoints) for code in read-only paging
#     regions; software breakpoints need write access to the page.

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO=${ISO:-"${REPO_ROOT}/build/Ultimate_Amiga_OS.iso"}
GDB_PORT=${GDB_PORT:-1234}
SERIAL_LOG=${SERIAL_LOG:-/tmp/uaos_serial.log}
OVMF_VARS=${OVMF_VARS:-/tmp/ovmf_vars_debug.fd}

if [ ! -f "$ISO" ]; then
    echo "ERROR: ISO not found at $ISO — run scripts/build_iso.sh first."
    exit 1
fi

# Fresh OVMF vars each run — stale vars can change boot order and drop
# to the UEFI shell instead of booting from CD.
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$OVMF_VARS"

echo "UAOS debug boot — GDB stub on :$GDB_PORT (CPU halted until 'continue')"
echo "  ISO    : $ISO"
echo "  Serial : $SERIAL_LOG"
echo "  Attach : gdb ${REPO_ROOT}/build/uaos-kernel.elf -ex 'target remote :$GDB_PORT'"
echo ""

qemu-system-x86_64 \
  -machine q35,usb=off \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file="$OVMF_VARS" \
  -device piix3-ide,id=ide \
  -drive if=none,id=cdrom,media=cdrom,file="$ISO" \
  -device ide-cd,drive=cdrom,bus=ide.0 \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-modern=on \
  -serial "file:${SERIAL_LOG}" \
  -m 512M -vga virtio -no-reboot -no-shutdown \
  -gdb tcp::"$GDB_PORT" -S
