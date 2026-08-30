#!/bin/bash
# create_disk.sh - Create a qcow2 disk image for UAOS

set -e

DISK_SIZE=${1:-1G}
DISK_PATH=${2:-/home/reaver/workspaces/uaos/uaos/build/uaos_disk.qcow2}

echo "Creating qcow2 disk image..."
echo "  Size: $DISK_SIZE"
echo "  Path: $DISK_PATH"

# Create the qcow2 image
qemu-img create -f qcow2 "$DISK_PATH" "$DISK_SIZE"

echo "Disk image created successfully: $DISK_PATH"
echo ""
echo "To use this disk with QEMU, add the following to your QEMU command:"
echo "  -drive file=$DISK_PATH,if=virtio,format=qcow2"
