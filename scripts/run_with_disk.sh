#!/bin/bash
# run_with_disk.sh — Run UAOS with a qcow2 disk attached
#
# Network modes (set NET= environment variable):
#   NET=user   (default, RECOMMENDED) — QEMU NAT/user-mode; UAOS gets 10.0.2.15 via DHCP
#   NET=bridge           — TAP bridged to host eth0; UAOS DHCPs from your LAN
#
# NOTE: Bridged mode requires manual setup and is currently unstable.
#       Use NAT (user-mode) unless you specifically need bridged networking.
#
# Bridge mode setup (run once, requires sudo):
#   sudo bash scripts/net_bridge_setup.sh up
#
# Bridge mode teardown:
#   sudo bash scripts/net_bridge_setup.sh down

set -e

DISK_PATH=${1:-/home/reaver/uaos/build/uaos_disk.qcow2}
OVMF_VARS=/tmp/ovmf_vars.fd
NET=${NET:-user}
TAP=${TAP:-tap0}

# Always copy fresh OVMF vars — stale vars can change the boot order
# and cause the firmware to drop to the UEFI shell instead of booting
# from CD.
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$OVMF_VARS"

echo "Starting UAOS with disk: $DISK_PATH  (network: $NET)"
if [ "$NET" = "bridge" ]; then
    echo "WARNING: Bridged networking is unstable. Consider using NAT (NET=user) instead."
fi
echo ""

# Build network device arguments
if [ "$NET" = "bridge" ]; then
    if ! ip link show "${TAP}" &>/dev/null; then
        echo "ERROR: TAP device '${TAP}' not found."
        echo "Run:  sudo bash scripts/net_bridge_setup.sh up"
        exit 1
    fi
    NETDEV_ARGS="-netdev tap,id=n0,ifname=${TAP},script=no,downscript=no"
else
    # user-mode NAT: QEMU built-in DHCP serves 10.0.2.15
    NETDEV_ARGS="-netdev user,id=n0"
fi

SERIAL_LOG=/tmp/uaos_serial.log
echo "Serial debug log: $SERIAL_LOG"

qemu-system-x86_64 \
  -machine q35,usb=off \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file="$OVMF_VARS" \
  -device piix3-ide,id=ide \
  -drive if=none,id=cdrom,media=cdrom,file=/home/reaver/uaos/build/Ultimate_Amiga_OS.iso \
  -device ide-cd,drive=cdrom,bus=ide.0 \
  -device virtio-blk-pci,disable-modern=on,drive=blk0 \
  -drive id=blk0,file="$DISK_PATH",if=none,format=qcow2 \
  ${NETDEV_ARGS} -device virtio-net-pci,netdev=n0 \
  -serial "file:${SERIAL_LOG}" \
  -m 512M -vga virtio -no-reboot -no-shutdown
