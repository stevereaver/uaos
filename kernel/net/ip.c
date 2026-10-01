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
#include "../klog/klog.h"

static ipv4_t   g_my_ip   = 0;
static ipv4_t   g_gateway = 0;
static ipv4_t   g_netmask = 0;
static uint8_t  g_my_mac[ETH_ALEN];
static uint16_t g_ip_id   = 1;

extern volatile uint64_t g_pit_ticks;      /* 100 Hz */

/* -------------------------------------------------------------------------
 * ARP-miss pending queue (UAOS-167)
 *
 * A send whose next-hop isn't in the ARP cache used to drop the packet
 * outright — the first DNS query to the gateway was always lost and cost
 * the full DNS retry timeout (~2 s at boot).  Instead we keep a small
 * pool of complete L2 frames awaiting resolution, flush them when the
 * ARP reply arrives (arp_rx -> ip_arp_resolved), and throttle ARP
 * requests per next-hop so a burst doesn't spam the wire.
 * ------------------------------------------------------------------------- */
#define ARP_PEND_MAX      8     /* queued frames total */
#define ARP_PEND_PER_PEER 2     /* max frames held per next-hop */
#define ARP_PEND_TTL      300   /* ticks (~3 s) a queued frame may wait */
#define ARP_REQ_MIN_GAP   100   /* ticks (~1 s) between requests per nexthop */

typedef struct {
    uint8_t  valid;
    ipv4_t   nexthop;
    uint16_t len;               /* bytes of frame[] in use */
    uint64_t tstamp;            /* enqueue tick (for expiry) */
    uint8_t  frame[ETH_HDR_LEN + IP_HDR_LEN + 1500];
} ArpPend;

static ArpPend g_pend[ARP_PEND_MAX];

/* Per-next-hop ARP request throttle (send at most one per ARP_REQ_MIN_GAP). */
static struct { ipv4_t ip; uint64_t tick; } g_arp_req[4];

static void arp_request_throttled(ipv4_t nexthop)
{
    int oldest = 0;
    for (int i = 0; i < 4; i++) {
        if (g_arp_req[i].ip == nexthop) {
            if (g_pit_ticks - g_arp_req[i].tick < ARP_REQ_MIN_GAP)
                return;
            g_arp_req[i].tick = g_pit_ticks;
            arp_request(nexthop);
            return;
        }
        if (g_arp_req[i].tick < g_arp_req[oldest].tick) oldest = i;
    }
    g_arp_req[oldest].ip   = nexthop;
    g_arp_req[oldest].tick = g_pit_ticks;
    arp_request(nexthop);
}

/* Queue the frame behind an outstanding ARP resolution.  Drops the
 * oldest frame for the same next-hop when that peer's share is full,
 * then the globally oldest expired frame, then the oldest overall. */
static int arp_pend_enqueue(ipv4_t nexthop, const uint8_t *frame, uint16_t len)
{
    int free_slot = -1, peer_oldest = -1, exp_oldest = -1, oldest = -1;
    int peer_count = 0;
    uint64_t t_peer = ~0ULL, t_exp = ~0ULL, t_any = ~0ULL;

    for (int i = 0; i < ARP_PEND_MAX; i++) {
        ArpPend *p = &g_pend[i];
        if (!p->valid) { if (free_slot < 0) free_slot = i; continue; }
        if (p->nexthop == nexthop) {
            peer_count++;
            if (p->tstamp < t_peer) { t_peer = p->tstamp; peer_oldest = i; }
        }
        if (g_pit_ticks - p->tstamp >= ARP_PEND_TTL && p->tstamp < t_exp) {
            t_exp = p->tstamp; exp_oldest = i;
        }
        if (p->tstamp < t_any) { t_any = p->tstamp; oldest = i; }
    }

    int slot;
    if (peer_count >= ARP_PEND_PER_PEER && peer_oldest >= 0)
        slot = peer_oldest;               /* bound per-peer memory */
    else if (free_slot >= 0)
        slot = free_slot;
    else if (exp_oldest >= 0)
        slot = exp_oldest;                /* reclaim an expired frame */
    else if (oldest >= 0)
        slot = oldest;
    else
        return 0;

    ArpPend *p = &g_pend[slot];
    p->valid   = 1;
    p->nexthop = nexthop;
    p->len     = len;
    p->tstamp  = g_pit_ticks;
    net_memcpy(p->frame, frame, len);
    return 1;
}

void ip_arp_resolved(ipv4_t ip)
{
    uint8_t mac[ETH_ALEN];
    if (!arp_lookup(ip, mac)) return;

    for (int i = 0; i < ARP_PEND_MAX; i++) {
        ArpPend *p = &g_pend[i];
        if (!p->valid || p->nexthop != ip) continue;
        p->valid = 0;
        /* The frame was queued before its Ethernet header was built —
         * fill it now (dst = resolved MAC) and transmit. */
        eth_build(p->frame, mac, g_my_mac, ETHERTYPE_IP,
                  (uint16_t)(p->len - ETH_HDR_LEN));
        netdev_send(p->frame, p->len);
    }
}

void ip_init(ipv4_t my_ip, ipv4_t gateway, ipv4_t netmask)
{
    g_my_ip   = my_ip;
    g_gateway = gateway;
    g_netmask = netmask;
    netdev_get_mac(g_my_mac);
    arp_set_gateway(gateway);
    net_memset(g_pend, 0, sizeof(g_pend));
    net_memset(g_arp_req, 0, sizeof(g_arp_req));
}

ipv4_t ip_get_local(void)   { return g_my_ip;   }
ipv4_t ip_get_gateway(void) { return g_gateway; }
ipv4_t ip_get_netmask(void) { return g_netmask; }

void ip_rx(const uint8_t *pkt, uint16_t len)
{
    if (len < IP_HDR_LEN) return;
    const IpHdr *h = (const IpHdr *)pkt;
    if ((h->ver_ihl >> 4) != 4) return;          /* IPv4 only */
    uint8_t  ihl      = (h->ver_ihl & 0x0F) * 4;
    uint16_t tot_len  = net_ntohs(h->tot_len);

    /* Per-packet serial debug removed — it produced ~180k lines of output
     * that blocked the CPU for ~20 minutes at 115200 baud, starving the
     * PS/2 mouse IRQ (IRQ 12, lower priority than E1000's IRQ 11 on the
     * slave PIC) and freezing the UI.  Only errors are logged now. */

    if (tot_len > len || ihl < IP_HDR_LEN) {
        klog_puts(KLOG_NET, KLOG_DEBUG, "rx: bad length\n");
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
        klog_puts(KLOG_NET, KLOG_DEBUG, "rx: bad cksum calc="); klog_appendf(KLOG_NET, KLOG_DEBUG, "0x%08X", calc);
        klog_puts(KLOG_NET, KLOG_DEBUG, " saved="); klog_appendf(KLOG_NET, KLOG_DEBUG, "0x%08X", saved); klog_putc(KLOG_NET, KLOG_DEBUG, '\n');
        return;
    }

    /* Drop fragments */
    uint16_t frag = net_ntohs(h->frag_off);
    if (frag & 0x3FFF) {
        klog_puts(KLOG_NET, KLOG_DEBUG, "rx: fragment dropped\n");
        return;
    }

    ipv4_t src_ip = net_ntohl(h->src);
    const uint8_t *payload = pkt + ihl;
    uint16_t plen = (uint16_t)(tot_len - ihl);

    switch (h->protocol) {
    case IP_PROTO_ICMP:
        icmp_rx(src_ip, payload, plen);
        break;
    case IP_PROTO_UDP:
        udp_rx(src_ip, payload, plen);
        break;
    case IP_PROTO_TCP:
        tcp_rx(src_ip, net_ntohl(h->dst), payload, plen);
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
        /* ARP miss — queue the frame and resolve in the background
         * instead of dropping it (UAOS-167).  arp_rx() flushes the queue
         * via ip_arp_resolved() when the reply lands; the throttled
         * request keeps a TX burst from ARP-spamming the wire. */
        arp_pend_enqueue(nexthop, frame, (uint16_t)(ETH_HDR_LEN + total_ip));
        arp_request_throttled(nexthop);
        return 1;
    }

    eth_build(frame, dst_mac, g_my_mac, ETHERTYPE_IP, total_ip);
    return netdev_send(frame, (uint16_t)(ETH_HDR_LEN + total_ip));
}
