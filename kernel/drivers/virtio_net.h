/*
 * virtio_net.h — UAOS VirtIO Network Device Driver
 *
 * Supports the legacy VirtIO 0.9.5 PCI network device (device ID 0x1000)
 * and the modern VirtIO 1.0 PCI network device (device ID 0x1041).
 * Both are presented by QEMU with -netdev user,id=n0 -device virtio-net-pci.
 *
 * Provides raw Ethernet TX/RX. Upper layers (ARP, IP, TCP) call into here.
 */

#ifndef UAOS_VIRTIO_NET_H
#define UAOS_VIRTIO_NET_H

#include <stdint.h>
#include <stddef.h>

/* Maximum Ethernet frame size (excluding VirtIO net header) */
#define VIRTIO_NET_MTU          1514
/* VirtIO net header size (legacy) */
#define VIRTIO_NET_HDR_SIZE     10

/* Receive buffer size: one header + one max frame */
#define VIRTIO_NET_RX_BUFSZ     (VIRTIO_NET_HDR_SIZE + VIRTIO_NET_MTU + 2)

/* Number of descriptors per virtqueue (must be power of 2) */
#define VIRTQ_SIZE              64

/* MAC address length */
#define ETH_ALEN                6

/* Callback type: called from IRQ context when a packet arrives */
typedef void (*virtio_net_rx_cb)(const uint8_t *data, uint16_t len);

/* Initialise the VirtIO-Net device. Returns 1 on success, 0 if not found. */
int  virtio_net_init(void);

/* Register a receive callback. Called for every inbound Ethernet frame. */
void virtio_net_set_rx_callback(virtio_net_rx_cb cb);

/* Transmit an Ethernet frame. data points to the raw frame (no VirtIO hdr).
 * Returns 1 on success, 0 on failure. */
int  virtio_net_send(const uint8_t *data, uint16_t len);

/* Copy the device MAC address into buf (must be ETH_ALEN bytes). */
void virtio_net_get_mac(uint8_t *buf);

/* Poll RX queue — call from event loop or timer tick if no IRQ */
void virtio_net_poll(void);

/* Returns 1 if the device was found and initialised */
int  virtio_net_is_up(void);

/* Set up IRQ handler (call after IDT + PIC initialised) */
void virtio_net_setup_irq(void);

#endif /* UAOS_VIRTIO_NET_H */
