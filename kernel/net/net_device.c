/*
 * net_device.c — UAOS Network Device Registry and VirtIO-Net Adapter
 *
 * Two responsibilities:
 *
 * 1. Global device registry: keeps a pointer to the one active NetDevice
 *    and exposes the netdev_* convenience wrappers used by the stack.
 *
 * 2. VirtIO-Net adapter: a static NetDevice instance that wraps the
 *    existing virtio_net.c driver.  This is the default device registered
 *    at boot.  When a real hardware driver is written it simply provides
 *    its own NetDevice and calls netdev_register() instead.
 */

#include "net_device.h"
#include "../drivers/virtio_net.h"

/* -------------------------------------------------------------------------
 * Global registry
 * ------------------------------------------------------------------------- */

static NetDevice *g_netdev = 0;
static int        g_up     = 0;

void netdev_register(NetDevice *dev)
{
    g_netdev = dev;
    g_up     = 0;   /* cleared; set to 1 after successful netdev_init() */
}

int netdev_is_up(void)  { return g_up; }

int netdev_init(void)
{
    if (!g_netdev || !g_netdev->init) return 0;
    int ok = g_netdev->init(g_netdev);
    if (ok) g_up = 1;
    return ok;
}

void netdev_get_mac(uint8_t *buf)
{
    if (g_netdev && g_netdev->get_mac)
        g_netdev->get_mac(g_netdev, buf);
}

int netdev_send(const uint8_t *data, uint16_t len)
{
    if (!g_up || !g_netdev || !g_netdev->send) return 0;
    return g_netdev->send(g_netdev, data, len);
}

void netdev_poll(void)
{
    if (g_up && g_netdev && g_netdev->poll)
        g_netdev->poll(g_netdev);
}

void netdev_set_rx_callback(netdev_rx_fn cb)
{
    if (g_netdev && g_netdev->set_rx_callback)
        g_netdev->set_rx_callback(g_netdev, cb);
}

void netdev_setup_irq(void)
{
    if (g_netdev && g_netdev->setup_irq)
        g_netdev->setup_irq(g_netdev);
}

/* -------------------------------------------------------------------------
 * VirtIO-Net adapter
 *
 * Wraps virtio_net.c behind the NetDevice interface.
 * virtio_net_set_rx_callback takes a bare function pointer of the same
 * type as netdev_rx_fn — the cast below is safe as the signatures match.
 * -------------------------------------------------------------------------
 */

static int  vnet_init(NetDevice *dev)
{
    (void)dev;
    return virtio_net_init();
}

static void vnet_get_mac(NetDevice *dev, uint8_t *buf)
{
    (void)dev;
    virtio_net_get_mac(buf);
}

static int vnet_send(NetDevice *dev, const uint8_t *data, uint16_t len)
{
    (void)dev;
    return virtio_net_send(data, len);
}

static void vnet_poll(NetDevice *dev)
{
    (void)dev;
    virtio_net_poll();
}

static void vnet_set_rx_callback(NetDevice *dev, netdev_rx_fn cb)
{
    (void)dev;
    /* virtio_net_rx_cb and netdev_rx_fn are identical in signature */
    virtio_net_set_rx_callback((virtio_net_rx_cb)cb);
}

static void vnet_setup_irq(NetDevice *dev)
{
    (void)dev;
    virtio_net_setup_irq();
}

/* The one static VirtIO-Net device instance */
static NetDevice g_virtio_net_device = {
    .name             = "virtio-net",
    .init             = vnet_init,
    .get_mac          = vnet_get_mac,
    .send             = vnet_send,
    .poll             = vnet_poll,
    .set_rx_callback  = vnet_set_rx_callback,
    .setup_irq        = vnet_setup_irq,
    .priv             = 0,
};

/*
 * netdev_register_virtio_net() — register the built-in VirtIO-Net adapter.
 * Called from net_stack_init() before netdev_init().
 * When a real hardware driver exists, call netdev_register(&your_device)
 * instead and this function need not be called.
 */
void netdev_register_virtio_net(void)
{
    netdev_register(&g_virtio_net_device);
}
