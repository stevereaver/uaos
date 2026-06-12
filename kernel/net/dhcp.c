/*
 * dhcp.c — Minimal DHCP client
 *
 * RFC 2131 / RFC 2132 DHCP client:
 *   DISCOVER (broadcast) → OFFER → REQUEST → ACK
 *
 * Uses the UDP layer (port 68 client, port 67 server).
 * All communication is via raw netdev_send / eth_rx since the
 * IP layer is not yet up at the time DHCP runs.
 *
 * We send directly via netdev_send with broadcast frames, and
 * poll netdev_poll() to receive replies into a local RX buffer
 * registered as a temporary rx_callback.
 */
#include "dhcp.h"
#include "net.h"
#include "eth.h"
#include "net_device.h"

/* -------------------------------------------------------------------------
 * DHCP packet layout (RFC 2131)
 * ------------------------------------------------------------------------- */
#define DHCP_MAGIC       0x63825363UL

#define DHCPDISCOVER  1
#define DHCPOFFER     2
#define DHCPREQUEST   3
#define DHCPACK       5
#define DHCPNAK       6

/* DHCP option codes */
#define OPT_SUBNET_MASK     1
#define OPT_ROUTER          3
#define OPT_DNS             6
#define OPT_REQUESTED_IP   50
#define OPT_LEASE_TIME     51
#define OPT_MSG_TYPE       53
#define OPT_SERVER_ID      54
#define OPT_PARAM_REQ      55
#define OPT_END           255

typedef struct __attribute__((packed)) {
    uint8_t  op;        /* 1=BOOTREQUEST */
    uint8_t  htype;     /* 1=Ethernet */
    uint8_t  hlen;      /* 6 */
    uint8_t  hops;
    uint32_t xid;       /* transaction ID */
    uint16_t secs;
    uint16_t flags;     /* 0x8000 = broadcast */
    uint32_t ciaddr;    /* client IP (0 for DISCOVER) */
    uint32_t yiaddr;    /* your IP (filled by server) */
    uint32_t siaddr;    /* server IP */
    uint32_t giaddr;    /* relay agent */
    uint8_t  chaddr[16];/* client hardware address */
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[308];
} DhcpPkt;

#define DHCP_PKT_MIN  (sizeof(DhcpPkt) - 308 + 4)  /* header + magic + END */
#define UDP_HDR_LEN   8
#define IP_HDR_LEN    20
#define ETH_HDR_LEN   14

/* -------------------------------------------------------------------------
 * Temporary RX state
 * ------------------------------------------------------------------------- */
static volatile int     g_dhcp_got    = 0;
static DhcpPkt          g_dhcp_reply;
static uint8_t          g_dhcp_mac[ETH_ALEN];
static uint32_t         g_dhcp_xid    = 0xDEADBEEFUL;
static ipv4_t           g_server_ip   = 0;

/* -------------------------------------------------------------------------
 * Busy-wait ~N ms
 * ------------------------------------------------------------------------- */
static void dhcp_delay_ms(uint32_t ms)
{
    volatile uint64_t n = (uint64_t)ms * 100000ULL;
    while (n--) __asm__ volatile("pause");
}

/* -------------------------------------------------------------------------
 * DHCP RX callback — registered temporarily during negotiation
 * Parses raw Ethernet frames and extracts DHCP replies.
 * ------------------------------------------------------------------------- */
static void dhcp_rx_cb(const uint8_t *frame, uint16_t len)
{
    if (len < ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN + sizeof(DhcpPkt) - 308)
        return;

    /* Check ethertype = IP */
    uint16_t et = (uint16_t)((frame[12]<<8)|frame[13]);
    if (et != 0x0800) return;

    const uint8_t *ip = frame + ETH_HDR_LEN;
    if ((ip[0] >> 4) != 4) return;
    uint8_t ihl = (ip[0] & 0xF) * 4;
    if (ip[9] != 17) return;   /* not UDP */

    const uint8_t *udp = ip + ihl;
    uint16_t src_port = (uint16_t)((udp[0]<<8)|udp[1]);
    uint16_t dst_port = (uint16_t)((udp[2]<<8)|udp[3]);
    if (src_port != 67 || dst_port != 68) return;

    const uint8_t *payload = udp + UDP_HDR_LEN;
    uint16_t payload_len = (uint16_t)(len - ETH_HDR_LEN - ihl - UDP_HDR_LEN);
    if (payload_len < 240) return;

    const DhcpPkt *p = (const DhcpPkt *)payload;
    if (p->op != 2) return;              /* BOOTREPLY */
    if (net_ntohl(p->magic) != DHCP_MAGIC) return;
    if (net_ntohl(p->xid) != g_dhcp_xid) return;

    net_memcpy(&g_dhcp_reply, p, sizeof(g_dhcp_reply));
    g_dhcp_got = 1;
}

/* -------------------------------------------------------------------------
 * Build and send a DHCP packet
 * ------------------------------------------------------------------------- */
static void dhcp_send(uint8_t msg_type, ipv4_t requested_ip, ipv4_t server_ip)
{
    /* Ethernet + IP + UDP + DHCP frame */
    uint8_t frame[ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN + sizeof(DhcpPkt)];
    net_memset(frame, 0, sizeof(frame));

    /* ---- DHCP payload ---- */
    DhcpPkt *dhcp = (DhcpPkt *)(frame + ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN);
    dhcp->op    = 1;  /* BOOTREQUEST */
    dhcp->htype = 1;
    dhcp->hlen  = ETH_ALEN;
    dhcp->hops  = 0;
    dhcp->xid   = net_htonl(g_dhcp_xid);
    dhcp->secs  = 0;
    dhcp->flags = net_htons(0x8000);  /* broadcast flag */
    dhcp->ciaddr = 0;
    dhcp->yiaddr = 0;
    dhcp->siaddr = 0;
    dhcp->giaddr = 0;
    net_memcpy(dhcp->chaddr, g_dhcp_mac, ETH_ALEN);
    dhcp->magic = net_htonl(DHCP_MAGIC);

    /* Options */
    uint8_t *opt = dhcp->options;
    *opt++ = OPT_MSG_TYPE; *opt++ = 1; *opt++ = msg_type;

    if (requested_ip) {
        *opt++ = OPT_REQUESTED_IP; *opt++ = 4;
        uint32_t rip = net_htonl(requested_ip);
        net_memcpy(opt, &rip, 4); opt += 4;
    }
    if (server_ip) {
        *opt++ = OPT_SERVER_ID; *opt++ = 4;
        uint32_t sip = net_htonl(server_ip);
        net_memcpy(opt, &sip, 4); opt += 4;
    }
    /* Parameter request list */
    *opt++ = OPT_PARAM_REQ; *opt++ = 3;
    *opt++ = OPT_SUBNET_MASK;
    *opt++ = OPT_ROUTER;
    *opt++ = OPT_DNS;
    *opt++ = OPT_END;

    uint16_t dhcp_len = (uint16_t)sizeof(DhcpPkt);

    /* ---- UDP header ---- */
    uint8_t *udp = frame + ETH_HDR_LEN + IP_HDR_LEN;
    udp[0] = 0;  udp[1] = 68;   /* src port 68 */
    udp[2] = 0;  udp[3] = 67;   /* dst port 67 */
    uint16_t udp_len = (uint16_t)(UDP_HDR_LEN + dhcp_len);
    udp[4] = (uint8_t)(udp_len >> 8); udp[5] = (uint8_t)(udp_len);
    udp[6] = 0; udp[7] = 0;          /* checksum = 0 (optional for UDP) */

    /* ---- IP header ---- */
    uint8_t *ip = frame + ETH_HDR_LEN;
    uint16_t ip_tot = (uint16_t)(IP_HDR_LEN + udp_len);
    ip[0]  = 0x45;
    ip[1]  = 0;
    ip[2]  = (uint8_t)(ip_tot >> 8); ip[3] = (uint8_t)(ip_tot);
    ip[4]  = 0; ip[5] = 1;  /* id */
    ip[6]  = 0; ip[7] = 0;  /* frag off */
    ip[8]  = 64;             /* TTL */
    ip[9]  = 17;             /* UDP */
    ip[10] = 0; ip[11] = 0; /* checksum (fill below) */
    ip[12] = 0; ip[13] = 0; ip[14] = 0; ip[15] = 0;   /* src 0.0.0.0 */
    ip[16] = 255; ip[17] = 255; ip[18] = 255; ip[19] = 255; /* dst broadcast */
    /* IP checksum */
    uint16_t ck = inet_cksum(ip, IP_HDR_LEN);
    ip[10] = (uint8_t)(ck >> 8); ip[11] = (uint8_t)(ck);

    /* ---- Ethernet header ---- */
    net_memset(frame, 0xFF, ETH_ALEN);          /* dst broadcast */
    net_memcpy(frame + ETH_ALEN, g_dhcp_mac, ETH_ALEN);
    frame[12] = 0x08; frame[13] = 0x00;

    netdev_send(frame, (uint16_t)(ETH_HDR_LEN + ip_tot));
}

/* -------------------------------------------------------------------------
 * Parse DHCP options from the reply
 * ------------------------------------------------------------------------- */
static void parse_offer(const DhcpPkt *p, DhcpLease *lease)
{
    lease->ip      = net_ntohl(p->yiaddr);
    lease->netmask = IPV4(255,255,255,0);
    lease->gateway = 0;
    lease->dns     = 0;
    lease->lease_secs = 86400;

    const uint8_t *opt = p->options;
    const uint8_t *end = opt + 308;
    while (opt < end && *opt != OPT_END) {
        uint8_t code = *opt++;
        if (code == 0) continue;
        if (opt >= end) break;
        uint8_t olen = *opt++;
        if (opt + olen > end) break;
        switch (code) {
        case OPT_SUBNET_MASK:
            if (olen == 4) lease->netmask = net_ntohl((uint32_t)opt[0]<<24|(uint32_t)opt[1]<<16|(uint32_t)opt[2]<<8|opt[3]);
            break;
        case OPT_ROUTER:
            if (olen >= 4) lease->gateway = net_ntohl((uint32_t)opt[0]<<24|(uint32_t)opt[1]<<16|(uint32_t)opt[2]<<8|opt[3]);
            break;
        case OPT_DNS:
            if (olen >= 4) lease->dns = net_ntohl((uint32_t)opt[0]<<24|(uint32_t)opt[1]<<16|(uint32_t)opt[2]<<8|opt[3]);
            break;
        case OPT_LEASE_TIME:
            if (olen == 4) lease->lease_secs = (uint32_t)opt[0]<<24|(uint32_t)opt[1]<<16|(uint32_t)opt[2]<<8|opt[3];
            break;
        case OPT_SERVER_ID:
            if (olen == 4) g_server_ip = net_ntohl((uint32_t)opt[0]<<24|(uint32_t)opt[1]<<16|(uint32_t)opt[2]<<8|opt[3]);
            break;
        }
        opt += olen;
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
int dhcp_request(DhcpLease *lease, uint32_t timeout_ms)
{
    netdev_get_mac(g_dhcp_mac);
    g_dhcp_got = 0;
    g_server_ip = 0;

    /* Register temporary RX callback */
    netdev_set_rx_callback(dhcp_rx_cb);

    /* --- Phase 1: DISCOVER --- */
    dhcp_send(DHCPDISCOVER, 0, 0);

    uint32_t waited = 0;
    while (!g_dhcp_got && waited < timeout_ms) {
        dhcp_delay_ms(10);
        netdev_poll();
        waited += 10;
    }
    if (!g_dhcp_got) return 0;

    /* Check it's an OFFER */
    {
        const uint8_t *opt = g_dhcp_reply.options;
        const uint8_t *end = opt + 308;
        int is_offer = 0;
        while (opt < end && *opt != OPT_END) {
            uint8_t code = *opt++;
            if (code == 0) continue;
            uint8_t olen = *opt++;
            if (code == OPT_MSG_TYPE && olen == 1 && *opt == DHCPOFFER)
                is_offer = 1;
            opt += olen;
        }
        if (!is_offer) return 0;
    }

    parse_offer(&g_dhcp_reply, lease);
    ipv4_t offered_ip = lease->ip;

    /* --- Phase 2: REQUEST --- */
    g_dhcp_got = 0;
    dhcp_send(DHCPREQUEST, offered_ip, g_server_ip);

    waited = 0;
    while (!g_dhcp_got && waited < timeout_ms) {
        dhcp_delay_ms(10);
        netdev_poll();
        waited += 10;
    }
    if (!g_dhcp_got) return 0;

    /* Check for ACK */
    {
        const uint8_t *opt = g_dhcp_reply.options;
        const uint8_t *end = opt + 308;
        while (opt < end && *opt != OPT_END) {
            uint8_t code = *opt++;
            if (code == 0) continue;
            uint8_t olen = *opt++;
            if (code == OPT_MSG_TYPE && olen == 1) {
                if (*opt == DHCPNAK) return 0;
                if (*opt == DHCPACK) { parse_offer(&g_dhcp_reply, lease); return 1; }
            }
            opt += olen;
        }
    }
    return 0;
}

int dhcp_renew(DhcpLease *lease, uint32_t timeout_ms)
{
    g_dhcp_got = 0;
    netdev_set_rx_callback(dhcp_rx_cb);
    dhcp_send(DHCPREQUEST, lease->ip, g_server_ip);

    uint32_t waited = 0;
    while (!g_dhcp_got && waited < timeout_ms) {
        dhcp_delay_ms(10);
        netdev_poll();
        waited += 10;
    }
    if (!g_dhcp_got) return 0;
    parse_offer(&g_dhcp_reply, lease);
    return 1;
}
