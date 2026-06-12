/*
 * stack.c — TCP/IP stack initialisation and top-level glue
 */
#include "stack.h"
#include "eth.h"
#include "arp.h"
#include "ip.h"
#include "tcp.h"
#include "dhcp.h"
#include "net_device.h"

static int    g_up        = 0;
static ipv4_t g_ip        = 0;
static int    g_dhcp_used = 0;

/* RX callback registered with VirtIO-Net driver after stack is up */
static void rx_callback(const uint8_t *frame, uint16_t len)
{
    eth_rx(frame, len);
}

int net_stack_init_ex(ipv4_t fallback_ip, ipv4_t fallback_gw,
                      ipv4_t fallback_nm, uint32_t dhcp_timeout_ms)
{
    /* Auto-probe: tries e1000 first, then virtio-net.  Each candidate's
     * init() is called inside netdev_probe(); g_up is set to 1 only if a
     * driver initialises successfully.  Callers may pre-register a specific
     * driver with netdev_register()+netdev_init() to bypass probe. */
    if (!netdev_is_up()) netdev_probe();
    if (!netdev_is_up()) return 0;

    uint8_t mac[ETH_ALEN];
    netdev_get_mac(mac);

    ipv4_t ip = fallback_ip;
    ipv4_t gw = fallback_gw;
    ipv4_t nm = fallback_nm;
    g_dhcp_used = 0;

    if (dhcp_timeout_ms > 0) {
        DhcpLease lease;
        if (dhcp_request(&lease, dhcp_timeout_ms)) {
            ip = lease.ip;
            gw = lease.gateway ? lease.gateway : fallback_gw;
            nm = lease.netmask ? lease.netmask : fallback_nm;
            g_dhcp_used = 1;
        }
        /* DHCP temporarily installed its own rx_callback — restore ours */
    }

    g_ip = ip;
    arp_init(ip, mac);
    ip_init(ip, gw, nm);
    netdev_set_rx_callback(rx_callback);
    netdev_setup_irq();
    g_up = 1;
    return 1;
}

int net_stack_init(ipv4_t ip, ipv4_t gateway, ipv4_t netmask)
{
    /* Try DHCP for up to 5 seconds; fall back to static only if no reply.
     * NAT (QEMU/VirtualBox) replies within ~50 ms; bridged mode with a
     * real router can take 1-2 s to respond (observed: ~1.4 s via PiHole).
     * The DHCP client retries DISCOVER every 1.5 s within this window. */
    return net_stack_init_ex(ip, gateway, netmask, 5000);
}

int  net_stack_is_up(void)    { return g_up; }
int  net_stack_dhcp_used(void){ return g_dhcp_used; }
void net_stack_poll(void)     { if (g_up) netdev_poll(); }
void net_stack_tick(void)     { if (g_up) tcp_tick(); }
ipv4_t net_stack_get_ip(void) { return g_ip; }

int net_stack_dhcp_renew(uint32_t timeout_ms)
{
    if (!g_up) return 0;
    uint8_t mac[ETH_ALEN];
    netdev_get_mac(mac);
    DhcpLease lease;
    if (dhcp_request(&lease, timeout_ms)) {
        g_ip = lease.ip;
        ipv4_t gw = lease.gateway;
        ipv4_t nm = lease.netmask ? lease.netmask : IPV4(255,255,255,0);
        arp_init(g_ip, mac);
        ip_init(g_ip, gw, nm);
        netdev_set_rx_callback(rx_callback);
        g_dhcp_used = 1;
        return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * IP <-> string conversion
 * ------------------------------------------------------------------------- */

static void u8_to_dec(uint8_t v, char *buf, int *pos)
{
    char tmp[4]; int i = 0;
    if (!v) { buf[(*pos)++] = '0'; return; }
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) buf[(*pos)++] = tmp[--i];
}

void net_ip_to_str(ipv4_t ip, char *buf)
{
    int pos = 0;
    u8_to_dec((uint8_t)(ip >> 24), buf, &pos); buf[pos++] = '.';
    u8_to_dec((uint8_t)(ip >> 16), buf, &pos); buf[pos++] = '.';
    u8_to_dec((uint8_t)(ip >>  8), buf, &pos); buf[pos++] = '.';
    u8_to_dec((uint8_t)(ip      ), buf, &pos);
    buf[pos] = '\0';
}

int net_str_to_ip(const char *s, ipv4_t *out)
{
    uint32_t result = 0;
    for (int octet = 0; octet < 4; octet++) {
        if (!*s) return 0;
        uint32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s++ - '0'); digits++; }
        if (!digits || v > 255) return 0;
        result = (result << 8) | v;
        if (octet < 3) { if (*s != '.') return 0; s++; }
    }
    *out = result;
    return 1;
}
