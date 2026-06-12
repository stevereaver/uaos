/*
 * udp.c — UDP socket implementation
 */
#include "udp.h"
#include "ip.h"

static UdpSocket g_socks[UDP_MAX_SOCKETS];
static uint16_t  g_ephemeral = 49152;

static int sock_valid(int s){ return s >= 0 && s < UDP_MAX_SOCKETS && g_socks[s].active; }

static uint16_t alloc_port(void)
{
    if (g_ephemeral >= 65535) g_ephemeral = 49152;
    return g_ephemeral++;
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
            g_socks[i].last_src_ip   = src_ip;
            g_socks[i].last_src_port = net_ntohs(h->src_port);
            ring_put(&g_socks[i], data, data_len);
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
    if (src_ip_out)   *src_ip_out   = g_socks[sock].last_src_ip;
    if (src_port_out) *src_port_out = g_socks[sock].last_src_port;
    return ring_get(&g_socks[sock], buf, maxlen);
}
