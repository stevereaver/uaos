#!/bin/bash
# net_bridge_setup.sh — Set up a TAP/bridge for UAOS VirtIO-Net
#
# Creates:
#   br0      — Linux bridge bridging eth0 + tap0
#   tap0     — TAP device owned by the current user (for QEMU)
#
# Run once before launching QEMU (requires sudo).
# The bridge lets UAOS DHCP directly from your LAN router.
#
# To tear down:   sudo bash net_bridge_setup.sh down
#
# Usage:
#   sudo bash scripts/net_bridge_setup.sh [up|down]

set -e

HOST_IF="${HOST_IF:-eth0}"
BRIDGE="${BRIDGE:-br0}"
TAP="${TAP:-tap0}"
OWNER="${SUDO_USER:-$(logname 2>/dev/null || echo $USER)}"

case "${1:-up}" in
up)
    echo "[bridge] Setting up ${BRIDGE} (${HOST_IF} + ${TAP}), tap owner: ${OWNER}"

    # Install bridge-utils if missing
    if ! command -v brctl &>/dev/null; then
        echo "[bridge] Installing bridge-utils..."
        apt-get install -y bridge-utils iproute2 2>/dev/null || true
    fi

    # Create TAP device owned by the calling user
    if ! ip link show "${TAP}" &>/dev/null; then
        ip tuntap add dev "${TAP}" mode tap user "${OWNER}"
        echo "[bridge] Created TAP: ${TAP} (owner ${OWNER})"
    else
        echo "[bridge] TAP ${TAP} already exists"
    fi

    # Create bridge
    if ! ip link show "${BRIDGE}" &>/dev/null; then
        ip link add name "${BRIDGE}" type bridge
        echo "[bridge] Created bridge: ${BRIDGE}"
    else
        echo "[bridge] Bridge ${BRIDGE} already exists"
    fi

    # Capture existing IP/gateway on host interface before adding to bridge
    HOST_IP=$(ip -4 addr show "${HOST_IF}" | awk '/inet /{print $2}' | head -1)
    HOST_GW=$(ip route show default dev "${HOST_IF}" | awk '/default/{print $3}' | head -1)

    # Add host interface + TAP to bridge
    ip link set "${HOST_IF}" master "${BRIDGE}" 2>/dev/null || true
    ip link set "${TAP}"     master "${BRIDGE}" 2>/dev/null || true

    # Bring everything up
    ip link set "${TAP}"    up
    ip link set "${BRIDGE}" up

    # Move host IP to bridge if there was one
    if [ -n "${HOST_IP}" ]; then
        ip addr flush dev "${HOST_IF}" 2>/dev/null || true
        ip addr add "${HOST_IP}" dev "${BRIDGE}" 2>/dev/null || true
        if [ -n "${HOST_GW}" ]; then
            ip route add default via "${HOST_GW}" dev "${BRIDGE}" 2>/dev/null || true
        fi
        echo "[bridge] Moved ${HOST_IP} from ${HOST_IF} to ${BRIDGE}"
    fi

    echo "[bridge] Ready. QEMU should use: -netdev tap,id=n0,ifname=${TAP},script=no,downscript=no"
    echo "[bridge]   UAOS will DHCP from your LAN router."
    ;;

down)
    echo "[bridge] Tearing down ${BRIDGE} / ${TAP}"

    # Restore host IP from bridge to eth0
    BRIDGE_IP=$(ip -4 addr show "${BRIDGE}" 2>/dev/null | awk '/inet /{print $2}' | head -1)
    BRIDGE_GW=$(ip route show default dev "${BRIDGE}" 2>/dev/null | awk '/default/{print $3}' | head -1)

    ip link set "${TAP}"     nomaster 2>/dev/null || true
    ip link set "${HOST_IF}" nomaster 2>/dev/null || true

    ip link set "${BRIDGE}" down 2>/dev/null || true
    ip link del "${BRIDGE}" 2>/dev/null || true
    ip link del "${TAP}"    2>/dev/null || true

    if [ -n "${BRIDGE_IP}" ]; then
        ip addr add "${BRIDGE_IP}" dev "${HOST_IF}" 2>/dev/null || true
        if [ -n "${BRIDGE_GW}" ]; then
            ip route add default via "${BRIDGE_GW}" dev "${HOST_IF}" 2>/dev/null || true
        fi
        echo "[bridge] Restored ${BRIDGE_IP} to ${HOST_IF}"
    fi

    echo "[bridge] Done."
    ;;

*)
    echo "Usage: $0 [up|down]"
    exit 1
    ;;
esac
