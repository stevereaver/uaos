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
 * Serial debug (COM1 = 0x3F8)
 * ------------------------------------------------------------------------- */
static inline void _dh_outb(uint16_t p, uint8_t v)
{ __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t _dh_inb(uint16_t p)
{ uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static void _dh_putc(char c)
{
    while ((_dh_inb(0x3FD) & 0x20) == 0) {}
    _dh_outb(0x3F8, (uint8_t)c);
    if (c == '\n') { while ((_dh_inb(0x3FD) & 0x20) == 0) {} _dh_outb(0x3F8, '\r'); }
}
static void _dh_puts(const char *s) { while (*s) _dh_putc(*s++); }
static void _dh_phex(uint32_t v) {
    static const char h[] = "0123456789ABCDEF";
    _dh_puts("0x");
    for (int i = 28; i >= 0; i -= 4) _dh_putc(h[(v >> i) & 0xF]);
}

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
static uint32_t         g_dhcp_xid    = 0;
static ipv4_t           g_server_ip   = 0;

/* -------------------------------------------------------------------------
 * rdtsc-based timing
 * We calibrate once against a known ~50ms busy-loop, then use rdtsc for
 * all subsequent delays so timing is accurate regardless of CPU speed.
 * ------------------------------------------------------------------------- */
static uint64_t g_tsc_hz = 0;   /* TSC ticks per second, 0 = uncalibrated */

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Calibrate TSC against PIT channel 2 (speaker timer).
 * Programs PIT ch2 for a one-shot count of 59659 ticks @ 1.193182 MHz
 * = exactly 50 ms, measures TSC ticks elapsed, extrapolates to Hz.
 * Safe to call before interrupts are enabled. */
static void dhcp_calibrate_tsc(void)
{
    if (g_tsc_hz) return;

    /* Gate PIT channel 2: bit1=gate on, bit0=speaker off */
    uint8_t old;
    __asm__ volatile("inb $0x61,%0" : "=a"(old));
    __asm__ volatile("outb %0,$0x61" :: "a"((uint8_t)((old & ~0x02) | 0x01)));

    /* Programme ch2: mode 0 (one-shot), lsb+msb, binary */
    __asm__ volatile("outb %0,$0x43" :: "a"((uint8_t)0xB0));
    /* 59659 = 1193182 / 20  → 50 ms */
    __asm__ volatile("outb %0,$0x42" :: "a"((uint8_t)(59659 & 0xFF)));
    __asm__ volatile("outb %0,$0x42" :: "a"((uint8_t)(59659 >> 8)));

    /* Enable gate */
    __asm__ volatile("outb %0,$0x61" :: "a"((uint8_t)((old & ~0x02) | 0x01)));

    uint64_t t0 = rdtsc();

    /* Wait for OUT pin to go high (bit 5 of port 0x61) */
    uint8_t s;
    do { __asm__ volatile("inb $0x61,%0" : "=a"(s)); } while (!(s & 0x20));

    uint64_t t1 = rdtsc();

    /* Restore port 0x61 */
    __asm__ volatile("outb %0,$0x61" :: "a"(old));

    uint64_t diff = t1 - t0;   /* ticks in ~50 ms */
    g_tsc_hz = diff * 20;      /* extrapolate to 1 second */
    _dh_puts("[DHCP] TSC Hz="); _dh_phex((uint32_t)(g_tsc_hz >> 32));
    _dh_phex((uint32_t)g_tsc_hz); _dh_putc('\n');
}

static void dhcp_delay_ms(uint32_t ms)
{
    if (!g_tsc_hz) {
        /* Fallback before calibration: conservative busy-wait */
        volatile uint64_t n = (uint64_t)ms * 200000ULL;
        while (n--) __asm__ volatile("pause");
        return;
    }
    uint64_t end = rdtsc() + (g_tsc_hz * ms / 1000ULL);
    while (rdtsc() < end) __asm__ volatile("pause");
}

/* -------------------------------------------------------------------------
 * DHCP RX callback — registered temporarily during negotiation
 * Parses raw Ethernet frames and extracts DHCP replies.
 * ------------------------------------------------------------------------- */
static void dhcp_rx_cb(const uint8_t *frame, uint16_t len)
{
    /* Log every frame seen during DHCP so we can diagnose receive issues */
    _dh_puts("[DHCP] rx frame len="); _dh_phex(len);
    uint16_t et = (uint16_t)((frame[12]<<8)|frame[13]);
    _dh_puts(" et="); _dh_phex(et); _dh_putc('\n');

    /* Minimum size: ETH + IP + UDP + DHCP header (without full options) */
    uint16_t min_len = ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN + 240;
    if (len < min_len) { _dh_puts("[DHCP] rx: too short\n"); return; }
    if (et != 0x0800)  { _dh_puts("[DHCP] rx: not IP\n"); return; }

    const uint8_t *ip = frame + ETH_HDR_LEN;
    if ((ip[0] >> 4) != 4) { _dh_puts("[DHCP] rx: not IPv4\n"); return; }
    uint8_t ihl = (ip[0] & 0xF) * 4;
    if (ip[9] != 17) { _dh_puts("[DHCP] rx: not UDP (proto="); _dh_phex(ip[9]); _dh_puts(")\n"); return; }

    const uint8_t *udp = ip + ihl;
    uint16_t src_port = (uint16_t)((udp[0]<<8)|udp[1]);
    uint16_t dst_port = (uint16_t)((udp[2]<<8)|udp[3]);
    if (src_port != 67 || dst_port != 68) {
        _dh_puts("[DHCP] rx: wrong ports src="); _dh_phex(src_port);
        _dh_puts(" dst="); _dh_phex(dst_port); _dh_putc('\n');
        return;
    }

    const uint8_t *payload = udp + UDP_HDR_LEN;
    uint16_t payload_len = (uint16_t)(len - ETH_HDR_LEN - ihl - UDP_HDR_LEN);
    if (payload_len < 240) { _dh_puts("[DHCP] rx: payload too short\n"); return; }

    const DhcpPkt *p = (const DhcpPkt *)payload;
    if (p->op != 2) { _dh_puts("[DHCP] rx: not BOOTREPLY\n"); return; }
    if (net_ntohl(p->magic) != DHCP_MAGIC) { _dh_puts("[DHCP] rx: bad magic\n"); return; }
    if (net_ntohl(p->xid) != g_dhcp_xid) {
        _dh_puts("[DHCP] rx: xid mismatch got="); _dh_phex(net_ntohl(p->xid));
        _dh_puts(" want="); _dh_phex(g_dhcp_xid); _dh_putc('\n');
        return;
    }

    _dh_puts("[DHCP] rx: valid OFFER/ACK\n");
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

    uint16_t total = (uint16_t)(ETH_HDR_LEN + ip_tot);
    _dh_puts("[DHCP] send len="); _dh_phex(total);
    _dh_puts(" src=");
    for (int i = 0; i < 6; i++) {
        static const char hx[] = "0123456789ABCDEF";
        _dh_putc(hx[frame[6+i]>>4]); _dh_putc(hx[frame[6+i]&0xF]);
        if (i<5) _dh_putc(':');
    }
    _dh_puts(" dst=FF:FF:FF:FF:FF:FF");
    _dh_puts(" xid="); _dh_phex(g_dhcp_xid);
    _dh_putc('\n');
    netdev_send(frame, total);
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
    dhcp_calibrate_tsc();

    netdev_get_mac(g_dhcp_mac);
    g_dhcp_got = 0;
    g_server_ip = 0;

    /* Generate a random XID from TSC + MAC so repeated boots get different
     * transaction IDs. Some DHCP servers (e.g. dnsmasq/PiHole) silently
     * drop requests with a hardcoded/repeated XID. */
    {
        uint64_t tsc = rdtsc();
        g_dhcp_xid = (uint32_t)(tsc ^ (tsc >> 32))
                   ^ ((uint32_t)g_dhcp_mac[2] << 24)
                   ^ ((uint32_t)g_dhcp_mac[3] << 16)
                   ^ ((uint32_t)g_dhcp_mac[4] <<  8)
                   ^  (uint32_t)g_dhcp_mac[5];
        if (!g_dhcp_xid) g_dhcp_xid = 0x12345678;  /* never zero */
    }

    _dh_puts("[DHCP] starting, MAC=");
    static const char hx[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        _dh_putc(hx[g_dhcp_mac[i] >> 4]); _dh_putc(hx[g_dhcp_mac[i] & 0xF]);
        if (i < 5) _dh_putc(':');
    }
    _dh_puts(" timeout_ms="); _dh_phex(timeout_ms); _dh_putc('\n');

    /* Register temporary RX callback */
    netdev_set_rx_callback(dhcp_rx_cb);

    /* --- Phase 1: DISCOVER with retries ---
     * Send a DISCOVER every 500 ms until we get an OFFER or time out.
     * Real DHCP servers (especially through a bridge) may need 1-3 s
     * to respond after the NIC link comes up. */
    uint32_t waited = 0;
    uint32_t next_discover = 0;  /* send immediately on first iteration */
    while (!g_dhcp_got && waited < timeout_ms) {
        if (waited >= next_discover) {
            _dh_puts("[DHCP] sending DISCOVER (t="); _dh_phex(waited); _dh_puts(")\n");
            dhcp_send(DHCPDISCOVER, 0, 0);
            next_discover = waited + 500;  /* retry every 500 ms */
        }
        dhcp_delay_ms(10);
        netdev_poll();
        waited += 10;
    }
    if (!g_dhcp_got) {
        _dh_puts("[DHCP] no OFFER received\n");
        return 0;
    }
    _dh_puts("[DHCP] OFFER received\n");

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

    _dh_puts("[DHCP] offered IP="); _dh_phex(offered_ip);
    _dh_puts(" gw="); _dh_phex(lease->gateway); _dh_putc('\n');

    /* --- Phase 2: REQUEST --- */
    g_dhcp_got = 0;
    _dh_puts("[DHCP] sending REQUEST\n");
    dhcp_send(DHCPREQUEST, offered_ip, g_server_ip);

    waited = 0;
    while (!g_dhcp_got && waited < timeout_ms) {
        dhcp_delay_ms(10);
        netdev_poll();
        waited += 10;
    }
    if (!g_dhcp_got) {
        _dh_puts("[DHCP] no ACK received\n");
        return 0;
    }

    /* Check for ACK */
    {
        const uint8_t *opt = g_dhcp_reply.options;
        const uint8_t *end = opt + 308;
        while (opt < end && *opt != OPT_END) {
            uint8_t code = *opt++;
            if (code == 0) continue;
            uint8_t olen = *opt++;
            if (code == OPT_MSG_TYPE && olen == 1) {
                if (*opt == DHCPNAK) { _dh_puts("[DHCP] NAK\n"); return 0; }
                if (*opt == DHCPACK) {
                    parse_offer(&g_dhcp_reply, lease);
                    _dh_puts("[DHCP] ACK, IP="); _dh_phex(lease->ip);
                    _dh_puts(" gw="); _dh_phex(lease->gateway); _dh_putc('\n');
                    return 1;
                }
            }
            opt += olen;
        }
    }
    _dh_puts("[DHCP] no ACK msg type in reply\n");
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
