/*
 * ip.c — IPv4 layer
 */
#include "ip.h"
#include "eth.h"
#include "arp.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "net_device.h"

static ipv4_t   g_my_ip   = 0;
static ipv4_t   g_gateway = 0;
static ipv4_t   g_netmask = 0;
static uint8_t  g_my_mac[ETH_ALEN];
static uint16_t g_ip_id   = 1;

void ip_init(ipv4_t my_ip, ipv4_t gateway, ipv4_t netmask)
{
    g_my_ip   = my_ip;
    g_gateway = gateway;
    g_netmask = netmask;
    netdev_get_mac(g_my_mac);
}

ipv4_t ip_get_local(void)   { return g_my_ip;   }
ipv4_t ip_get_gateway(void) { return g_gateway; }
ipv4_t ip_get_netmask(void) { return g_netmask; }

/* Serial debug helpers */
static inline void _ip_outb(uint16_t p, uint8_t v)
{ __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t _ip_inb(uint16_t p)
{ uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static void _ip_putc(char c) {
    while ((_ip_inb(0x3FD) & 0x20) == 0) {}
    _ip_outb(0x3F8, (uint8_t)c);
    if (c == '\n') { while ((_ip_inb(0x3FD) & 0x20) == 0) {} _ip_outb(0x3F8, '\r'); }
}
static void _ip_puts(const char *s) { while (*s) _ip_putc(*s++); }
static void _ip_phex(uint32_t v) {
    static const char h[] = "0123456789ABCDEF";
    _ip_puts("0x");
    for (int i = 28; i >= 0; i -= 4) _ip_putc(h[(v >> i) & 0xF]);
}

void ip_rx(const uint8_t *pkt, uint16_t len)
{
    if (len < IP_HDR_LEN) return;
    const IpHdr *h = (const IpHdr *)pkt;
    if ((h->ver_ihl >> 4) != 4) return;          /* IPv4 only */
    uint8_t  ihl      = (h->ver_ihl & 0x0F) * 4;
    uint16_t tot_len  = net_ntohs(h->tot_len);

    _ip_puts("[IP] rx proto="); _ip_phex(h->protocol);
    _ip_puts(" tot_len="); _ip_phex(tot_len);
    _ip_puts(" len="); _ip_phex(len);
    _ip_puts(" ihl="); _ip_phex(ihl);
    _ip_putc('\n');

    if (tot_len > len || ihl < IP_HDR_LEN) {
        _ip_puts("[IP] rx: bad length\n");
        return;
    }

    /* Verify checksum.
     * inet_cksum() returns a value in host byte order (big-endian semantics).
     * The stored checksum in the packet is big-endian on the wire, which
     * x86 reads as a byte-swapped uint16_t.  Convert calc to network byte
     * order before comparing. */
    uint16_t saved = h->checksum;
    ((IpHdr *)h)->checksum = 0;
    uint16_t calc = net_htons(inet_cksum(h, ihl));
    ((IpHdr *)h)->checksum = saved;
    if (calc != saved) {
        _ip_puts("[IP] rx: bad cksum calc="); _ip_phex(calc);
        _ip_puts(" saved="); _ip_phex(saved); _ip_putc('\n');
        return;
    }

    /* Drop fragments */
    uint16_t frag = net_ntohs(h->frag_off);
    if (frag & 0x3FFF) {
        _ip_puts("[IP] rx: fragment dropped\n");
        return;
    }

    ipv4_t src_ip = net_ntohl(h->src);
    const uint8_t *payload = pkt + ihl;
    uint16_t plen = (uint16_t)(tot_len - ihl);

    _ip_puts("[IP] rx: dispatching proto="); _ip_phex(h->protocol); _ip_putc('\n');

    switch (h->protocol) {
    case IP_PROTO_ICMP:
        icmp_rx(src_ip, payload, plen);
        break;
    case IP_PROTO_UDP:
        udp_rx(src_ip, payload, plen);
        break;
    case IP_PROTO_TCP:
        tcp_rx(src_ip, payload, plen);
        break;
    default:
        break;
    }
}

int ip_send(ipv4_t dst_ip, uint8_t proto, uint8_t *payload, uint16_t payload_len)
{
    if (!netdev_is_up()) return 0;

    /* Build complete Ethernet frame buffer */
    uint8_t frame[ETH_HDR_LEN + IP_HDR_LEN + 1500];
    uint16_t total_ip = (uint16_t)(IP_HDR_LEN + payload_len);
    if (total_ip > (uint16_t)(IP_HDR_LEN + 1480)) return 0;

    /* Fill IP header */
    IpHdr *h = (IpHdr *)(frame + ETH_HDR_LEN);
    h->ver_ihl  = 0x45;
    h->dscp_ecn = 0;
    h->tot_len  = net_htons(total_ip);
    h->id       = net_htons(g_ip_id++);
    h->frag_off = 0;
    h->ttl      = 64;
    h->protocol = proto;
    h->checksum = 0;
    h->src      = net_htonl(g_my_ip);
    h->dst      = net_htonl(dst_ip);
    h->checksum = net_htons(inet_cksum(h, IP_HDR_LEN));

    /* Copy payload */
    net_memcpy(frame + ETH_HDR_LEN + IP_HDR_LEN, payload, payload_len);

    /* Determine next-hop MAC (same subnet or gateway) */
    ipv4_t nexthop = dst_ip;
    if ((dst_ip & g_netmask) != (g_my_ip & g_netmask))
        nexthop = g_gateway;

    /* Special case: broadcast */
    uint8_t dst_mac[ETH_ALEN];
    if (dst_ip == 0xFFFFFFFF || dst_ip == (g_my_ip | ~g_netmask)) {
        net_memcpy(dst_mac, ETH_BCAST, ETH_ALEN);
    } else if (!arp_lookup(nexthop, dst_mac)) {
        /* Not in ARP cache — send ARP request (packet is dropped for now) */
        arp_request(nexthop);
        return 0;
    }

    eth_build(frame, dst_mac, g_my_mac, ETHERTYPE_IP, total_ip);
    return netdev_send(frame, (uint16_t)(ETH_HDR_LEN + total_ip));
}
