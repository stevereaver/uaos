/*
 * sky2.h — UAOS Marvell Yukon-2 (Sky2) Gigabit Ethernet Driver
 *
 * Supports the Marvell 88E8058 "Yukon-2 EC Ultra" found in the
 * MacBookPro4,1 (PCI 11ab:436a), plus related Yukon-2 family
 * device IDs.
 *
 * BAR0: 64-bit MMIO register space (16 KB window).
 * MAC address: read from B2_MAC_1 register block.
 *
 * Public API mirrors e1000.h so the net_device adapter is trivial.
 */

#ifndef UAOS_SKY2_H
#define UAOS_SKY2_H

#include <stdint.h>
#include <stddef.h>

/* Maximum Ethernet frame size (no FCS) */
#define SKY2_MTU            1514

/* Receive buffer size — one full MTU frame */
#define SKY2_RX_BUFSZ       2048

/* RX callback type */
typedef void (*sky2_rx_cb)(const uint8_t *data, uint16_t len);

/* Initialise the sky2 device.  Returns 1 on success, 0 if not found. */
int  sky2_init(void);

/* Returns 1 if the device was found and initialised. */
int  sky2_is_up(void);

/* Copy the device MAC address into buf[ETH_ALEN]. */
void sky2_get_mac(uint8_t *buf);

/* Transmit one raw Ethernet frame.  Returns 1 on success, 0 on failure. */
int  sky2_send(const uint8_t *data, uint16_t len);

/* Drain the status ring: process RX completions and TX indices,
 * invoke the RX callback for each frame. */
void sky2_poll(void);

/* Register the function called for every received Ethernet frame. */
void sky2_set_rx_callback(sky2_rx_cb cb);

/* Register the IRQ handler with the unified IRQ layer. */
void sky2_setup_irq(void);

#endif /* UAOS_SKY2_H */
