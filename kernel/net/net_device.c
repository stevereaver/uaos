/*
 * net_device.c — UAOS Network Device Registry, VirtIO-Net and e1000 Adapters
 *
 * Three responsibilities:
 *
 * 1. Global device registry: keeps a pointer to the one active NetDevice
 *    and exposes the netdev_* convenience wrappers used by the stack.
 *
 * 2. VirtIO-Net adapter: wraps virtio_net.c behind the NetDevice interface.
 *    Used when running under QEMU with -device virtio-net-pci.
 *
 * 3. e1000 adapter: wraps e1000.c behind the NetDevice interface.
 *    Used when running under VirtualBox (Intel PRO/1000 MT) or QEMU
 *    with -device e1000.
 *
 * netdev_probe() tries e1000 first (real/virtual hardware), then falls back
 * to virtio-net.  net_stack_init_ex() calls netdev_probe() automatically.
 */

#include "net_device.h"
#include "../drivers/virtio_net.h"
#include "../drivers/e1000.h"

/* -------------------------------------------------------------------------
 * Serial debug helpers (COM1 = 0x3F8) — mirrors the ones in e1000.c
 * ------------------------------------------------------------------------- */
static inline void _nd_outb(uint16_t p, uint8_t v)
{
    __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p));
}
static inline uint8_t _nd_inb(uint16_t p)
{
    uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v;
}
static void _nd_putc(char c)
{
    while ((_nd_inb(0x3FD) & 0x20) == 0) {}
    _nd_outb(0x3F8, (uint8_t)c);
    if (c == '\n') { while ((_nd_inb(0x3FD) & 0x20) == 0) {} _nd_outb(0x3F8, '\r'); }
}
static void _nd_puts(const char *s) { while (*s) _nd_putc(*s++); }

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

const char *netdev_name(void)
{
    if (g_netdev && g_netdev->name) return g_netdev->name;
    return "unknown";
}

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
 */
void netdev_register_virtio_net(void)
{
    netdev_register(&g_virtio_net_device);
}

/* -------------------------------------------------------------------------
 * e1000 adapter
 *
 * Wraps e1000.c behind the NetDevice interface.
 * e1000_rx_cb and netdev_rx_fn have identical signatures so the cast is safe.
 * -------------------------------------------------------------------------
 */

static int e1k_init(NetDevice *dev)
{
    (void)dev;
    return e1000_init();
}

static void e1k_get_mac(NetDevice *dev, uint8_t *buf)
{
    (void)dev;
    e1000_get_mac(buf);
}

static int e1k_send(NetDevice *dev, const uint8_t *data, uint16_t len)
{
    (void)dev;
    return e1000_send(data, len);
}

static void e1k_poll(NetDevice *dev)
{
    (void)dev;
    e1000_poll();
}

static void e1k_set_rx_callback(NetDevice *dev, netdev_rx_fn cb)
{
    (void)dev;
    e1000_set_rx_callback((e1000_rx_cb)cb);
}

static void e1k_setup_irq(NetDevice *dev)
{
    (void)dev;
    e1000_setup_irq();
}

static NetDevice g_e1000_device = {
    .name            = "e1000",
    .init            = e1k_init,
    .get_mac         = e1k_get_mac,
    .send            = e1k_send,
    .poll            = e1k_poll,
    .set_rx_callback = e1k_set_rx_callback,
    .setup_irq       = e1k_setup_irq,
    .priv            = 0,
};

void netdev_register_e1000(void)
{
    netdev_register(&g_e1000_device);
}

/* -------------------------------------------------------------------------
 * netdev_probe() — try all known drivers, register the first found.
 *
 * Priority order:
 *   1. e1000  — Intel PRO/1000 (VirtualBox, QEMU -device e1000)
 *   2. virtio-net — QEMU -device virtio-net-pci (default QEMU)
 *
 * The actual hardware detection happens inside each driver's init(), so
 * we register the candidate and then let netdev_init() call init().
 * If init() returns 0 we try the next candidate.
 *
 * Note: this function sets the registered device but does NOT call
 * netdev_init() — net_stack_init_ex() does that separately.
 * -------------------------------------------------------------------------
 */
void netdev_probe(void)
{
    /* Try e1000 first. */
    _nd_puts("[NETDEV] probing e1000...\n");
    netdev_register(&g_e1000_device);
    if (netdev_init()) {
        _nd_puts("[NETDEV] e1000 selected\n");
        return;
    }

    /* Fall back to VirtIO-Net — must call netdev_init() here too. */
    _nd_puts("[NETDEV] e1000 not found, trying virtio-net...\n");
    netdev_register(&g_virtio_net_device);
    if (netdev_init()) {
        _nd_puts("[NETDEV] virtio-net selected\n");
        return;
    }

    _nd_puts("[NETDEV] no network device found\n");
}
