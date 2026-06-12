/*
 * net_device.h — UAOS Network Device Interface
 *
 * This is the hardware abstraction layer between the TCP/IP stack and the
 * underlying network hardware driver.  It is modelled loosely on the Amiga
 * SANA-II (Standard Amiga Networking Architecture) concept: the stack and
 * all tools talk to a net_device, not to a specific driver.
 *
 * To add a new hardware driver (e.g. RTL8139, ne2000):
 *   1. Fill in a NetDevice struct with your function pointers.
 *   2. Call netdev_register(&your_device) before net_stack_init().
 *   3. Nothing else in the stack needs to change.
 *
 * Flow:
 *   cmd_ping / bsdsocket.library
 *         |
 *         v
 *   icmp/ip/arp/dhcp  (kernel/net/)
 *         |
 *         v
 *   net_device  (this interface)
 *         |
 *         v
 *   virtio_net  /  future real hardware driver
 */

#ifndef UAOS_NET_DEVICE_H
#define UAOS_NET_DEVICE_H

#include <stdint.h>

/* Ethernet MAC address length */
#ifndef ETH_ALEN
#define ETH_ALEN 6
#endif

/*
 * RX callback — called by the driver when a complete Ethernet frame arrives.
 * frame: pointer to raw Ethernet frame (no VirtIO header).
 * len:   frame length in bytes.
 * The callback is invoked from poll() or an IRQ handler.
 */
typedef void (*netdev_rx_fn)(const uint8_t *frame, uint16_t len);

/*
 * NetDevice — abstract network hardware interface.
 *
 * Drivers fill this struct and pass it to netdev_register().
 * All function pointers must be non-NULL after registration.
 */
typedef struct NetDevice {
    /* Human-readable driver name, e.g. "virtio-net", "rtl8139" */
    const char *name;

    /*
     * init() — power on and initialise the hardware.
     * Returns 1 on success, 0 if the device is not present or failed.
     * Called once by net_stack_init().
     */
    int  (*init)(struct NetDevice *dev);

    /*
     * get_mac() — copy the device MAC address into buf[ETH_ALEN].
     * Called after init() succeeds.
     */
    void (*get_mac)(struct NetDevice *dev, uint8_t *buf);

    /*
     * send() — transmit one raw Ethernet frame.
     * data: pointer to the frame (dst MAC first, no VirtIO header).
     * len:  frame length in bytes (max 1514).
     * Returns 1 on success, 0 on failure.
     */
    int  (*send)(struct NetDevice *dev, const uint8_t *data, uint16_t len);

    /*
     * poll() — check for and process received frames.
     * Should call rx_cb() for each complete frame found.
     * Called from the main event loop and from CMD_YIELD.
     * Must be safe to call from both polled and IRQ contexts.
     */
    void (*poll)(struct NetDevice *dev);

    /*
     * set_rx_callback() — register the function to call on frame arrival.
     * The stack calls this once after init(); DHCP calls it temporarily
     * with its own callback then restores the stack's callback.
     */
    void (*set_rx_callback)(struct NetDevice *dev, netdev_rx_fn cb);

    /*
     * setup_irq() — register the hardware IRQ handler.
     * Called after the IDT and PIC are fully initialised.
     * May be NULL for poll-only drivers.
     */
    void (*setup_irq)(struct NetDevice *dev);

    /* Private driver data — the driver may store anything here */
    void *priv;
} NetDevice;

/* -------------------------------------------------------------------------
 * Global device registry (single active device for now)
 * ------------------------------------------------------------------------- */

/*
 * Register a network device.  Must be called before net_stack_init().
 * Only one device is active at a time; a second call replaces the first.
 */
void netdev_register(NetDevice *dev);

/*
 * Convenience wrappers — these forward to the registered device.
 * The stack and tools use ONLY these; they never call virtio_net_* directly.
 */
int  netdev_init(void);
void netdev_get_mac(uint8_t *buf);
int  netdev_send(const uint8_t *data, uint16_t len);
void netdev_poll(void);
void netdev_set_rx_callback(netdev_rx_fn cb);
void netdev_setup_irq(void);
int  netdev_is_up(void);

/*
 * Register the built-in VirtIO-Net adapter as the active device.
 * Call this from net_stack_init() at boot.  When a real hardware driver is
 * added, call netdev_register(&your_device) instead of this.
 */
void netdev_register_virtio_net(void);

#endif /* UAOS_NET_DEVICE_H */
