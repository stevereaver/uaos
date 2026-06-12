/*
 * e1000.h — UAOS Intel 82540EM (e1000) Gigabit Ethernet Driver
 *
 * Supports the Intel 82540EM family as presented by VirtualBox
 * ("Intel PRO/1000 MT Desktop") and QEMU (-device e1000).
 *
 * PCI:  vendor 0x8086, device 0x100E (and related 8254x family)
 * BAR0: 128 KB MMIO register space (memory-mapped, 32-bit accesses)
 *
 * Public API mirrors virtio_net.h so the net_device adapter is trivial.
 */

#ifndef UAOS_E1000_H
#define UAOS_E1000_H

#include <stdint.h>
#include <stddef.h>

/* Maximum Ethernet frame size (no FCS) */
#define E1000_MTU           1514

/* MAC address length */
#ifndef ETH_ALEN
#define E1000_ETH_ALEN      6
#else
#define E1000_ETH_ALEN      ETH_ALEN
#endif

/* Receive buffer size — one full MTU frame */
#define E1000_RX_BUFSZ      2048

/* Descriptor ring sizes — must be multiples of 8 */
#define E1000_NUM_RX_DESC   32
#define E1000_NUM_TX_DESC   32

/* RX callback type */
typedef void (*e1000_rx_cb)(const uint8_t *data, uint16_t len);

/* Initialise the e1000 device.  Returns 1 on success, 0 if not found. */
int  e1000_init(void);

/* Returns 1 if the device was found and initialised. */
int  e1000_is_up(void);

/* Copy the device MAC address into buf[ETH_ALEN]. */
void e1000_get_mac(uint8_t *buf);

/* Transmit one raw Ethernet frame.  Returns 1 on success, 0 on failure. */
int  e1000_send(const uint8_t *data, uint16_t len);

/* Poll RX ring for received frames and invoke the RX callback. */
void e1000_poll(void);

/* Register the function called for every received Ethernet frame. */
void e1000_set_rx_callback(e1000_rx_cb cb);

/* Register the IRQ handler with the IDT/PIC (call after IDT is ready). */
void e1000_setup_irq(void);

#endif /* UAOS_E1000_H */
