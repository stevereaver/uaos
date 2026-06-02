#!/bin/bash
# run_with_disk.sh - Run UAOS with a qcow2 disk attached

set -e

DISK_PATH=${1:-/home/reaver/uaos/build/uaos_disk.qcow2}
OVMF_VARS=/tmp/ovmf_vars.fd

# Always copy fresh OVMF vars — stale vars can change the boot order
# and cause the firmware to drop to the UEFI shell instead of booting
# from CD.
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$OVMF_VARS"

echo "Starting UAOS with disk: $DISK_PATH"
echo ""

qemu-system-x86_64 \
  -machine q35,usb=off \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file="$OVMF_VARS" \
  -device piix3-ide,id=ide \
  -drive if=none,id=cdrom,media=cdrom,file=/home/reaver/uaos/build/Ultimate_Amiga_OS.iso \
  -device ide-cd,drive=cdrom,bus=ide.0 \
  -device virtio-blk-pci,disable-modern=on,drive=blk0 \
  -drive id=blk0,file="$DISK_PATH",if=none,format=qcow2 \
  -m 512M -vga virtio -no-reboot -no-shutdown
