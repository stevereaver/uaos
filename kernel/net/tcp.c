/*
 * tcp.c — TCP state machine
 *
 * Implements a minimal but correct TCP stack supporting:
 *   - Active connect (SYN → ESTABLISHED → data → FIN)
 *   - Passive listen/accept
 *   - Data send/receive with ACK
 *   - Connection teardown (FIN/FIN-ACK)
 *
 * No retransmit timer yet — relies on remote to resend on loss.
 * Single-segment send (no Nagle, no window splitting).
 */
#include "tcp.h"
#include "ip.h"

static TcpSocket g_socks[TCP_MAX_SOCKETS];
static uint32_t  g_isn_counter = 0x12345678;  /* initial seq number seed */

/* -------------------------------------------------------------------------
 * Ring buffer helpers
 * ------------------------------------------------------------------------- */
static void rbuf_put(uint8_t *buf, uint16_t *tail, uint16_t *head,
                     uint16_t size, const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        uint16_t next = (uint16_t)((*tail + 1) % size);
        if (next == *head) return;
        buf[*tail] = data[i];
        *tail = next;
    }
}
static int rbuf_get(uint8_t *buf, uint16_t *head, uint16_t *tail,
                    uint16_t size, uint8_t *out, uint16_t maxlen)
{
    int n = 0;
    while (n < maxlen && *head != *tail) {
        out[n++] = buf[*head];
        *head = (uint16_t)((*head + 1) % size);
    }
    return n;
}
static uint16_t rbuf_used(uint16_t head, uint16_t tail, uint16_t size)
{
    return (uint16_t)((tail - head + size) % size);
}

/* -------------------------------------------------------------------------
 * TCP checksum (requires IP pseudo-header)
 * ------------------------------------------------------------------------- */
static uint16_t tcp_checksum(ipv4_t src_ip, ipv4_t dst_ip,
                              const uint8_t *seg, uint16_t seg_len)
{
    /* Pseudo-header: src(4) dst(4) zero(1) proto(1) tcp_len(2) */
    uint8_t pseudo[12];
    uint32_t s = net_htonl(src_ip);
    uint32_t d = net_htonl(dst_ip);
    net_memcpy(pseudo + 0, &s, 4);
    net_memcpy(pseudo + 4, &d, 4);
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_TCP;
    uint16_t tl = net_htons(seg_len);
    net_memcpy(pseudo + 10, &tl, 2);

    uint32_t sum = 0;
    const uint16_t *p;
    /* Sum pseudo-header */
    p = (const uint16_t *)pseudo;
    for (int i = 0; i < 6; i++) sum += p[i];
    /* Sum TCP segment */
    p = (const uint16_t *)seg;
    uint16_t rem = seg_len;
    while (rem > 1) { sum += *p++; rem -= 2; }
    if (rem) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* -------------------------------------------------------------------------
 * Send a TCP segment
 * ------------------------------------------------------------------------- */
static void tcp_send_seg(TcpSocket *s, uint8_t flags,
                          const uint8_t *data, uint16_t data_len)
{
    uint8_t seg[TCP_HDR_LEN + 1460];
    if (data_len > 1460) data_len = 1460;
    uint16_t seg_len = (uint16_t)(TCP_HDR_LEN + data_len);

    TcpHdr *h = (TcpHdr *)seg;
    h->src_port  = net_htons(s->local_port);
    h->dst_port  = net_htons(s->remote_port);
    h->seq       = net_htonl(s->snd_nxt);
    h->ack       = (flags & TCP_ACK) ? net_htonl(s->rcv_nxt) : 0;
    h->data_off  = (TCP_HDR_LEN / 4) << 4;
    h->flags     = flags;
    h->window    = net_htons((uint16_t)(TCP_RX_BUF_SIZE - 1));
    h->checksum  = 0;
    h->urgent    = 0;

    if (data && data_len)
        net_memcpy(seg + TCP_HDR_LEN, data, data_len);

    h->checksum = tcp_checksum(s->local_ip, s->remote_ip, seg, seg_len);

    ip_send(s->remote_ip, IP_PROTO_TCP, seg, seg_len);

    /* Advance snd_nxt for data and SYN/FIN (each consumes 1 seq) */
    if (flags & (TCP_SYN | TCP_FIN)) s->snd_nxt++;
    s->snd_nxt += data_len;
}

/* -------------------------------------------------------------------------
 * Find a socket matching src/dst
 * ------------------------------------------------------------------------- */
static TcpSocket *find_sock(ipv4_t src_ip, uint16_t src_port,
                             uint16_t dst_port, int want_listen)
{
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        TcpSocket *s = &g_socks[i];
        if (s->state == TCP_CLOSED) continue;
        if (want_listen && s->state == TCP_LISTEN && s->local_port == dst_port)
            return s;
        if (!want_listen &&
            s->remote_ip == src_ip &&
            s->remote_port == src_port &&
            s->local_port == dst_port)
            return s;
    }
    return 0;
}

static TcpSocket *alloc_sock(void)
{
    for (int i = 0; i < TCP_MAX_SOCKETS; i++)
        if (g_socks[i].state == TCP_CLOSED) return &g_socks[i];
    return 0;
}

static int sock_idx(TcpSocket *s) { return (int)(s - g_socks); }

/* -------------------------------------------------------------------------
 * RX handler
 * ------------------------------------------------------------------------- */
void tcp_rx(ipv4_t src_ip, const uint8_t *pkt, uint16_t len)
{
    if (len < TCP_HDR_LEN) return;
    const TcpHdr *h = (const TcpHdr *)pkt;
    uint8_t  data_off = (uint8_t)((h->data_off >> 4) * 4);
    if (data_off < TCP_HDR_LEN || data_off > len) return;

    uint16_t src_port = net_ntohs(h->src_port);
    uint16_t dst_port = net_ntohs(h->dst_port);
    uint32_t seq      = net_ntohl(h->seq);
    uint32_t ack_num  = net_ntohl(h->ack);
    uint8_t  flags    = h->flags;
    const uint8_t *data = pkt + data_off;
    uint16_t data_len   = (uint16_t)(len - data_off);

    /* Find matching socket */
    TcpSocket *s = find_sock(src_ip, src_port, dst_port, 0);
    if (!s) {
        /* Check for listener */
        if (flags & TCP_SYN) {
            s = find_sock(src_ip, src_port, dst_port, 1);
            if (s) {
                /* Spawn new socket for this connection */
                TcpSocket *ns = alloc_sock();
                if (!ns) return;
                net_memset(ns, 0, sizeof(*ns));
                ns->state       = TCP_SYN_RECEIVED;
                ns->local_ip    = ip_get_local();
                ns->local_port  = dst_port;
                ns->remote_ip   = src_ip;
                ns->remote_port = src_port;
                ns->rcv_nxt     = seq + 1;
                ns->snd_nxt     = g_isn_counter;
                ns->snd_una     = g_isn_counter;
                g_isn_counter  += 0x10000;
                tcp_send_seg(ns, TCP_SYN | TCP_ACK, 0, 0);
                ns->snd_una = ns->snd_nxt;
            }
        }
        return;
    }

    /* Update ACK / window */
    if (flags & TCP_ACK) {
        s->snd_una = ack_num;
        s->snd_wnd = net_ntohs(h->window);
    }

    switch (s->state) {

    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            s->rcv_nxt = seq + 1;
            s->state   = TCP_ESTABLISHED;
            tcp_send_seg(s, TCP_ACK, 0, 0);
        } else if (flags & TCP_RST) {
            s->state = TCP_CLOSED;
        }
        break;

    case TCP_SYN_RECEIVED:
        if (flags & TCP_ACK) {
            s->state = TCP_ESTABLISHED;
        }
        break;

    case TCP_ESTABLISHED:
        if (flags & TCP_RST) { s->state = TCP_CLOSED; break; }
        /* Queue received data */
        if (data_len > 0) {
            rbuf_put(s->rx_buf, &s->rx_tail, &s->rx_head,
                     TCP_RX_BUF_SIZE, data, data_len);
            s->rcv_nxt += data_len;
            tcp_send_seg(s, TCP_ACK, 0, 0);
        }
        if (flags & TCP_FIN) {
            s->rcv_nxt++;
            s->state = TCP_CLOSE_WAIT;
            tcp_send_seg(s, TCP_ACK, 0, 0);
        }
        break;

    case TCP_FIN_WAIT_1:
        if (flags & TCP_ACK) s->state = TCP_FIN_WAIT_2;
        if (flags & TCP_FIN) {
            s->rcv_nxt++;
            tcp_send_seg(s, TCP_ACK, 0, 0);
            s->state = TCP_TIME_WAIT;
        }
        break;

    case TCP_FIN_WAIT_2:
        if (flags & TCP_FIN) {
            s->rcv_nxt++;
            tcp_send_seg(s, TCP_ACK, 0, 0);
            s->state = TCP_TIME_WAIT;
        }
        break;

    case TCP_LAST_ACK:
        if (flags & TCP_ACK) s->state = TCP_CLOSED;
        break;

    case TCP_TIME_WAIT:
        s->state = TCP_CLOSED;
        break;

    default:
        break;
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

int tcp_connect(ipv4_t dst_ip, uint16_t dst_port, uint16_t local_port)
{
    TcpSocket *s = alloc_sock();
    if (!s) return -1;
    net_memset(s, 0, sizeof(*s));
    s->state       = TCP_SYN_SENT;
    s->local_ip    = ip_get_local();
    s->local_port  = local_port ? local_port : (uint16_t)(49152 + sock_idx(s));
    s->remote_ip   = dst_ip;
    s->remote_port = dst_port;
    s->snd_nxt     = g_isn_counter;
    s->snd_una     = g_isn_counter;
    g_isn_counter += 0x10000;
    tcp_send_seg(s, TCP_SYN, 0, 0);
    return sock_idx(s);
}

int tcp_listen(uint16_t local_port)
{
    TcpSocket *s = alloc_sock();
    if (!s) return -1;
    net_memset(s, 0, sizeof(*s));
    s->state      = TCP_LISTEN;
    s->local_ip   = ip_get_local();
    s->local_port = local_port;
    return sock_idx(s);
}

int tcp_accept(int listen_sock)
{
    (void)listen_sock;
    /* Find the first SYN_RECEIVED or ESTABLISHED socket not in LISTEN */
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (g_socks[i].state == TCP_ESTABLISHED &&
            g_socks[i].local_port == g_socks[listen_sock].local_port)
            return i;
    }
    return -1;
}

int tcp_send(int sock, const uint8_t *data, uint16_t len)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return 0;
    TcpSocket *s = &g_socks[sock];
    if (s->state != TCP_ESTABLISHED) return 0;
    tcp_send_seg(s, TCP_PSH | TCP_ACK, data, len);
    return len;
}

int tcp_recv(int sock, uint8_t *buf, uint16_t maxlen)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return 0;
    TcpSocket *s = &g_socks[sock];
    return rbuf_get(s->rx_buf, &s->rx_head, &s->rx_tail,
                    TCP_RX_BUF_SIZE, buf, maxlen);
}

void tcp_close(int sock)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return;
    TcpSocket *s = &g_socks[sock];
    if (s->state == TCP_ESTABLISHED || s->state == TCP_SYN_RECEIVED) {
        s->state = TCP_FIN_WAIT_1;
        tcp_send_seg(s, TCP_FIN | TCP_ACK, 0, 0);
    } else if (s->state == TCP_CLOSE_WAIT) {
        s->state = TCP_LAST_ACK;
        tcp_send_seg(s, TCP_FIN | TCP_ACK, 0, 0);
    } else {
        s->state = TCP_CLOSED;
    }
}

TcpState tcp_state(int sock)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return TCP_CLOSED;
    return g_socks[sock].state;
}

void tcp_tick(void)
{
    /* Advance TIME_WAIT sockets to CLOSED after a tick */
    for (int i = 0; i < TCP_MAX_SOCKETS; i++)
        if (g_socks[i].state == TCP_TIME_WAIT)
            g_socks[i].state = TCP_CLOSED;
}
