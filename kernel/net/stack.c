/*
 * stack.c — TCP/IP stack initialisation and top-level glue
 */
#include "stack.h"
#include "eth.h"
#include "arp.h"
#include "ip.h"
#include "tcp.h"
#include "dhcp.h"
#include "../drivers/virtio_net.h"

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
    if (!virtio_net_init()) return 0;

    uint8_t mac[ETH_ALEN];
    virtio_net_get_mac(mac);

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
    virtio_net_set_rx_callback(rx_callback);
    virtio_net_setup_irq();
    g_up = 1;
    return 1;
}

int net_stack_init(ipv4_t ip, ipv4_t gateway, ipv4_t netmask)
{
    /* Try DHCP for up to 1 second; fall back to static if no reply */
    return net_stack_init_ex(ip, gateway, netmask, 1000);
}

int  net_stack_is_up(void)    { return g_up; }
int  net_stack_dhcp_used(void){ return g_dhcp_used; }
void net_stack_poll(void)     { if (g_up) virtio_net_poll(); }
void net_stack_tick(void)     { if (g_up) tcp_tick(); }
ipv4_t net_stack_get_ip(void) { return g_ip; }

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
