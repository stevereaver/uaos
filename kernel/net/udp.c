/*
 * udp.c — UDP socket implementation
 */
#include "udp.h"
#include "ip.h"
#include "../irq/irq.h"
#include "../drivers/entropy.h"

static UdpSocket g_socks[UDP_MAX_SOCKETS];
static uint16_t  g_ephemeral = 49152;

static int sock_valid(int s){ return s >= 0 && s < UDP_MAX_SOCKETS && g_socks[s].active; }

static int port_in_use(uint16_t port)
{
    for (int i = 0; i < UDP_MAX_SOCKETS; i++)
        if (g_socks[i].active && g_socks[i].local_port == port)
            return 1;
    return 0;
}

/* Ephemeral ports are drawn at random (UAOS-168): a predictable
 * sequential source port makes off-path DNS/NTP response spoofing
 * trivial.  Retry a few random draws, then fall back to the sequential
 * counter so a full table can't wedge the allocator. */
static uint16_t alloc_port(void)
{
    for (int tries = 0; tries < 16; tries++) {
        uint32_t r = 0;
        entropy_fill(&r, sizeof(r));
        uint16_t port = (uint16_t)(49152 + (r % (65536 - 49152)));
        if (!port_in_use(port)) return port;
    }
    do {
        if (g_ephemeral >= 65535) g_ephemeral = 49152;
        uint16_t port = g_ephemeral++;
        if (!port_in_use(port)) return port;
    } while (1);
}

static void ring_put(UdpSocket *s, const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        uint16_t next = (uint16_t)((s->rx_tail + 1) % UDP_RX_BUF_SIZE);
        if (next == s->rx_head) return; /* overflow, drop */
        s->rx_buf[s->rx_tail] = data[i];
        s->rx_tail = next;
    }
}

static int ring_get(UdpSocket *s, uint8_t *buf, uint16_t maxlen)
{
    int n = 0;
    while (n < maxlen && s->rx_head != s->rx_tail) {
        buf[n++] = s->rx_buf[s->rx_head];
        s->rx_head = (uint16_t)((s->rx_head + 1) % UDP_RX_BUF_SIZE);
    }
    return n;
}

static void ring_skip(UdpSocket *s, uint16_t n)
{
    s->rx_head = (uint16_t)((s->rx_head + n) % UDP_RX_BUF_SIZE);
}

static uint16_t ring_used(const UdpSocket *s)
{
    return (uint16_t)((s->rx_tail - s->rx_head + UDP_RX_BUF_SIZE)
                      % UDP_RX_BUF_SIZE);
}

static uint16_t ring_free(const UdpSocket *s)
{
    return (uint16_t)(UDP_RX_BUF_SIZE - 1 - ring_used(s));
}

/* Queue a whole datagram.  Queue and payload-ring capacity are checked
 * up front so a datagram is either queued intact or dropped whole —
 * a partial record would corrupt the FIFO's boundary accounting
 * (UAOS-185).  irq_save'd because the RX poll path and socket readers
 * can run on different preemptable tasks. */
static void queue_dgram(UdpSocket *s, ipv4_t src_ip, uint16_t src_port,
                        const uint8_t *data, uint16_t len)
{
    uint64_t fl = irq_save();
    if (s->rx_qcount < UDP_RX_QUEUE && ring_free(s) >= len) {
        uint8_t slot = (uint8_t)((s->rx_qhead + s->rx_qcount) % UDP_RX_QUEUE);
        s->rx_q[slot].src_ip   = src_ip;
        s->rx_q[slot].src_port = src_port;
        s->rx_q[slot].len      = len;
        s->rx_qcount++;
        ring_put(s, data, len);
        s->last_src_ip   = src_ip;
        s->last_src_port = src_port;
    }
    irq_restore(fl);
}

void udp_rx(ipv4_t src_ip, const uint8_t *pkt, uint16_t len)
{
    if (len < UDP_HDR_LEN) return;
    const UdpHdr *h = (const UdpHdr *)pkt;
    uint16_t dst_port = net_ntohs(h->dst_port);
    uint16_t data_len = (uint16_t)(net_ntohs(h->length) - UDP_HDR_LEN);
    if (data_len > len - UDP_HDR_LEN) return;
    const uint8_t *data = pkt + UDP_HDR_LEN;

    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (g_socks[i].active && g_socks[i].local_port == dst_port) {
            queue_dgram(&g_socks[i], src_ip, net_ntohs(h->src_port),
                        data, data_len);
            break;
        }
    }
}

int udp_open(uint16_t local_port)
{
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (!g_socks[i].active) {
            g_socks[i].active     = 1;
            g_socks[i].local_port = local_port ? local_port : alloc_port();
            g_socks[i].rx_head    = 0;
            g_socks[i].rx_tail    = 0;
            g_socks[i].rx_qhead   = 0;
            g_socks[i].rx_qcount  = 0;
            return i;
        }
    }
    return -1;
}

void udp_close(int sock)
{
    if (sock_valid(sock)) g_socks[sock].active = 0;
}

int udp_send(int sock, ipv4_t dst_ip, uint16_t dst_port,
             const uint8_t *data, uint16_t len)
{
    if (!sock_valid(sock)) return 0;
    uint8_t pkt[UDP_HDR_LEN + 1472];
    if (len > 1472) len = 1472;
    UdpHdr *h = (UdpHdr *)pkt;
    h->src_port = net_htons(g_socks[sock].local_port);
    h->dst_port = net_htons(dst_port);
    h->length   = net_htons((uint16_t)(UDP_HDR_LEN + len));
    h->checksum = 0;   /* optional for UDP */
    net_memcpy(pkt + UDP_HDR_LEN, data, len);
    return ip_send(dst_ip, IP_PROTO_UDP, pkt, (uint16_t)(UDP_HDR_LEN + len));
}

int udp_recv(int sock, uint8_t *buf, uint16_t maxlen,
             ipv4_t *src_ip_out, uint16_t *src_port_out)
{
    if (!sock_valid(sock)) return 0;
    UdpSocket *s = &g_socks[sock];

    /* Datagram semantics (UAOS-185): each call pops exactly one queued
     * datagram and reports that datagram's real peer — never bytes
     * spanning two packets.  An undersized buf truncates the datagram
     * and the unread tail is discarded, matching BSD recvfrom(). */
    uint64_t fl = irq_save();
    if (!s->rx_qcount) { irq_restore(fl); return 0; }

    UdpDgram *d = &s->rx_q[s->rx_qhead];
    uint16_t n = d->len < maxlen ? d->len : maxlen;
    ring_get(s, buf, n);
    if (d->len > n) ring_skip(s, (uint16_t)(d->len - n));

    if (src_ip_out)   *src_ip_out   = d->src_ip;
    if (src_port_out) *src_port_out = d->src_port;

    s->rx_qhead = (uint8_t)((s->rx_qhead + 1) % UDP_RX_QUEUE);
    s->rx_qcount--;
    irq_restore(fl);
    return n;
}

/* -------------------------------------------------------------------------
 * C:netstat — UDP socket table dump (UAOS-203).  Read-only.
 * ------------------------------------------------------------------------- */
#include "../dbg/diag.h"

void Udp_DiagDump(void *ctx, void (*emit)(void *, const char *))
{
    DiagLine l;
    int open = 0;
    emit(ctx, " udp# local-port  rx-queued  last-sender");
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        UdpSocket *s = &g_socks[i];
        if (!s->active) continue;
        open++;
        dl_reset(&l);
        dl_ch(&l, ' '); dl_dec(&l, (uint64_t)i); dl_pad(&l, 5);
        dl_dec(&l, s->local_port); dl_pad(&l, 17);
        dl_dec(&l, s->rx_qcount); dl_add(&l, "dg/");
        dl_dec(&l, ring_used(s)); dl_ch(&l, 'B'); dl_pad(&l, 28);
        if (s->last_src_ip) {
            dl_dec(&l, (s->last_src_ip >> 24) & 0xFF); dl_ch(&l, '.');
            dl_dec(&l, (s->last_src_ip >> 16) & 0xFF); dl_ch(&l, '.');
            dl_dec(&l, (s->last_src_ip >> 8) & 0xFF);  dl_ch(&l, '.');
            dl_dec(&l, s->last_src_ip & 0xFF);
            dl_ch(&l, ':'); dl_dec(&l, s->last_src_port);
        } else dl_add(&l, "-");
        dl_emit(&l, ctx, emit);
    }
    dl_reset(&l);
    dl_add(&l, " "); dl_dec(&l, (uint64_t)open); dl_add(&l, " open UDP socket(s)");
    dl_emit(&l, ctx, emit);
}
