/*
 * tcp.c — TCP state machine
 *
 * Implements a minimal but correct TCP stack supporting:
 *   - Active connect (SYN → ESTABLISHED → data → FIN)
 *   - Passive listen/accept
 *   - Data send/receive with ACK
 *   - Connection teardown (FIN/FIN-ACK)
 *   - Retransmit timer with exponential backoff (tcp_tick, called at 10 Hz)
 *   - Connect timeout (SYN_SENT), TIME_WAIT expiry, half-open cleanup
 *   - Peer-window enforcement and a single-segment in-flight limit on send
 *   - RFC 793 RST generation for segments that match no socket
 *
 * Single-segment send (no Nagle, no window splitting): at most one
 * seq-carrying segment is in flight per socket, so the single retx_buf
 * always holds exactly the segment that needs replaying.
 */
#include "tcp.h"
#include "ip.h"

static TcpSocket g_socks[TCP_MAX_SOCKETS];
static uint32_t  g_isn_counter = 0x12345678;  /* initial seq number seed */

static void tcp_retransmit(TcpSocket *s);   /* defined below tcp_rx */

/* TCP sequence comparison (wraparound-safe) */
static inline int seq_gt(uint32_t a, uint32_t b)
{ return (int32_t)(a - b) > 0; }

/* -------------------------------------------------------------------------
 * Ring buffer helpers
 * ------------------------------------------------------------------------- */
static uint16_t rbuf_put(uint8_t *buf, uint16_t *tail, uint16_t *head,
                         uint16_t size, const uint8_t *data, uint16_t len)
{
    uint16_t n = 0;
    while (n < len) {
        uint16_t next = (uint16_t)((*tail + 1) % size);
        if (next == *head) break;
        buf[*tail] = data[n++];
        *tail = next;
    }
    return n;
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
static uint16_t rbuf_free(uint16_t head, uint16_t tail, uint16_t size)
{
    return (uint16_t)(size - 1 - rbuf_used(head, tail, size));
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
    uint8_t seg[TCP_HDR_LEN + TCP_MSS];
    if (data_len > TCP_MSS) data_len = TCP_MSS;
    uint16_t seg_len = (uint16_t)(TCP_HDR_LEN + data_len);

    TcpHdr *h = (TcpHdr *)seg;
    h->src_port  = net_htons(s->local_port);
    h->dst_port  = net_htons(s->remote_port);
    h->seq       = net_htonl(s->snd_nxt);
    h->ack       = (flags & TCP_ACK) ? net_htonl(s->rcv_nxt) : 0;
    h->data_off  = (TCP_HDR_LEN / 4) << 4;
    h->flags     = flags;
    h->window    = net_htons(rbuf_free(s->rx_head, s->rx_tail,
                                       TCP_RX_BUF_SIZE));
    h->checksum  = 0;
    h->urgent    = 0;

    if (data && data_len)
        net_memcpy(seg + TCP_HDR_LEN, data, data_len);

    h->checksum = tcp_checksum(s->local_ip, s->remote_ip, seg, seg_len);

    /* Record snd_nxt BEFORE advancing, for retransmit replay */
    uint32_t seq_before = s->snd_nxt;

    ip_send(s->remote_ip, IP_PROTO_TCP, seg, seg_len);

    /* Advance snd_nxt for data and SYN/FIN (each consumes 1 seq) */
    if (flags & (TCP_SYN | TCP_FIN)) s->snd_nxt++;
    s->snd_nxt += data_len;

    /* Save segment for retransmit — but not for pure ACKs (nothing to replay) */
    int carries_seq = (flags & (TCP_SYN | TCP_FIN)) || data_len > 0;
    if (carries_seq) {
        s->retx_seq   = seq_before;
        s->retx_flags = flags;
        if (data && data_len && data_len <= TCP_RETX_BUF_SIZE) {
            net_memcpy(s->retx_buf, data, data_len);
            s->retx_len = data_len;
        } else {
            s->retx_len = 0;    /* SYN/FIN — no payload to store */
        }
        /* Arm the retransmit timer; count is NOT reset here — that happens
         * only when snd_una advances (i.e. the remote ACKs our data). */
        if (s->retx_count == 0)
            s->retx_timer = TCP_RETX_TICKS_INIT;
    }
}

/* -------------------------------------------------------------------------
 * Send a RST for a segment that matched no socket (RFC 793).
 * Caller supplies seq/ack: for an incoming segment carrying ACK the RST is
 * seq=SEG.ACK; otherwise seq=0 with RST|ACK acking SEG.SEQ+SEG.LEN.
 * ------------------------------------------------------------------------- */
static void tcp_send_reset(ipv4_t dst_ip, uint16_t dst_port,
                           uint16_t src_port, uint32_t seq, uint32_t ack,
                           uint8_t flags)
{
    uint8_t seg[TCP_HDR_LEN];
    TcpHdr *h = (TcpHdr *)seg;
    h->src_port = net_htons(src_port);
    h->dst_port = net_htons(dst_port);
    h->seq      = net_htonl(seq);
    h->ack      = net_htonl(ack);
    h->data_off = (TCP_HDR_LEN / 4) << 4;
    h->flags    = flags;
    h->window   = 0;
    h->checksum = 0;
    h->urgent   = 0;
    h->checksum = tcp_checksum(ip_get_local(), dst_ip, seg, TCP_HDR_LEN);
    ip_send(dst_ip, IP_PROTO_TCP, seg, TCP_HDR_LEN);
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
 * Queue inbound stream data.
 *
 * Only in-order bytes are accepted: a leading overlap (retransmit of bytes
 * we already have) is trimmed off, a segment ahead of rcv_nxt is dropped
 * whole, and a full ring takes only what fits.  rcv_nxt advances by bytes
 * actually queued, so the ACK sent here covers only delivered data — a
 * full ring drop-and-NACKs the tail for the peer to retransmit.
 * ------------------------------------------------------------------------- */
static void tcp_rx_data(TcpSocket *s, uint32_t seq,
                        const uint8_t *data, uint16_t data_len)
{
    int32_t ahead = (int32_t)(seq - s->rcv_nxt);
    if (ahead < 0) {
        uint32_t skip = (uint32_t)-ahead;
        if (skip >= data_len) {                 /* pure duplicate */
            tcp_send_seg(s, TCP_ACK, 0, 0);
            return;
        }
        data     += skip;
        data_len -= (uint16_t)skip;
    } else if (ahead > 0) {                     /* gap — keep nothing */
        tcp_send_seg(s, TCP_ACK, 0, 0);
        return;
    }
    s->rcv_nxt += rbuf_put(s->rx_buf, &s->rx_tail, &s->rx_head,
                           TCP_RX_BUF_SIZE, data, data_len);
    tcp_send_seg(s, TCP_ACK, 0, 0);
}

/* -------------------------------------------------------------------------
 * RX handler
 * ------------------------------------------------------------------------- */
void tcp_rx(ipv4_t src_ip, ipv4_t dst_ip, const uint8_t *pkt, uint16_t len)
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
                if (ns) {
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
                    return;
                }
                /* Socket table full — fall through and refuse the SYN */
            }
        }
        /* RFC 793: a segment addressed to us that matches no socket gets
         * a RST, so the peer sees "connection refused" instead of a
         * filtered port.  Never answer a RST with a RST. */
        if (!(flags & TCP_RST) && dst_ip == ip_get_local()) {
            if (flags & TCP_ACK) {
                tcp_send_reset(src_ip, src_port, dst_port,
                               ack_num, 0, TCP_RST);
            } else {
                tcp_send_reset(src_ip, src_port, dst_port, 0,
                               seq + data_len +
                               ((flags & (TCP_SYN | TCP_FIN)) ? 1u : 0u),
                               TCP_RST | TCP_ACK);
            }
        }
        return;
    }

    /* Update ACK / window */
    if (flags & TCP_ACK) {
        /* Only an ACK inside (snd_una, snd_nxt] may advance snd_una:
         * a stale or reordered ACK must not rewind it, and an ACK for
         * sequence space we never sent is ignored.  The advertised
         * window is still taken from any ACK (dup ACKs carry fresh
         * window information). */
        if (seq_gt(ack_num, s->snd_una) && !seq_gt(ack_num, s->snd_nxt)) {
            s->snd_una    = ack_num;
            s->retx_timer = 0;
            s->retx_count = 0;
            s->retx_len   = 0;
        }
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
        } else if (flags & TCP_SYN) {
            /* Duplicate SYN — our SYN-ACK was lost (e.g. dropped while
             * ARP resolved).  Replay the saved segment so the handshake
             * can still complete. */
            tcp_retransmit(s);
        }
        break;

    case TCP_ESTABLISHED:
        if (flags & TCP_RST) { s->state = TCP_CLOSED; break; }
        /* Queue received data; the FIN only counts once every byte
         * before it has actually been delivered to the ring. */
        if (data_len > 0)
            tcp_rx_data(s, seq, data, data_len);
        if ((flags & TCP_FIN) && seq + data_len == s->rcv_nxt) {
            s->rcv_nxt++;
            s->state = TCP_CLOSE_WAIT;
            tcp_send_seg(s, TCP_ACK, 0, 0);
        }
        break;

    case TCP_FIN_WAIT_1:
        if (data_len > 0)
            tcp_rx_data(s, seq, data, data_len);
        if (flags & TCP_ACK) s->state = TCP_FIN_WAIT_2;
        if ((flags & TCP_FIN) && seq + data_len == s->rcv_nxt) {
            s->rcv_nxt++;
            tcp_send_seg(s, TCP_ACK, 0, 0);
            s->state      = TCP_TIME_WAIT;
            s->conn_timer = 0;
        }
        break;

    case TCP_FIN_WAIT_2:
        if (data_len > 0)
            tcp_rx_data(s, seq, data, data_len);
        if ((flags & TCP_FIN) && seq + data_len == s->rcv_nxt) {
            s->rcv_nxt++;
            tcp_send_seg(s, TCP_ACK, 0, 0);
            s->state      = TCP_TIME_WAIT;
            s->conn_timer = 0;
        }
        break;

    case TCP_CLOSE_WAIT:
        /* The peer's stream already ended at its FIN — anything still
         * arriving is a retransmit of pre-FIN data (e.g. recovering a
         * tail we dropped while the ring was full).  Re-ACK it. */
        if (data_len > 0)
            tcp_rx_data(s, seq, data, data_len);
        else if (flags & TCP_FIN)
            tcp_send_seg(s, TCP_ACK, 0, 0);
        break;

    case TCP_LAST_ACK:
        if (flags & TCP_ACK) s->state = TCP_CLOSED;
        break;

    case TCP_TIME_WAIT:
        /* Restart the 2MSL timer if we get a retransmitted FIN */
        if (flags & TCP_FIN) {
            tcp_send_seg(s, TCP_ACK, 0, 0);
            s->conn_timer = 0;   /* reset wait */
        }
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
    if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT) return 0;
    if (len == 0 || s->fin_pending) return 0;   /* close already requested */

    /* One seq-carrying segment in flight at a time: retx_buf can hold
     * only a single segment, so sending while the previous one is still
     * unacked would leave a hole no retransmit could fill.  Return 0 and
     * let the caller poll/retry. */
    if (s->snd_una != s->snd_nxt) return 0;

    /* Honor the peer's advertised receive window. */
    uint32_t wnd = s->snd_wnd;
    if (wnd == 0) return 0;
    if (len > wnd)     len = (uint16_t)wnd;
    if (len > TCP_MSS) len = TCP_MSS;

    tcp_send_seg(s, TCP_PSH | TCP_ACK, data, len);
    return len;
}

int tcp_recv(int sock, uint8_t *buf, uint16_t maxlen)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return 0;
    TcpSocket *s = &g_socks[sock];
    uint16_t free_before = rbuf_free(s->rx_head, s->rx_tail, TCP_RX_BUF_SIZE);
    int n = rbuf_get(s->rx_buf, &s->rx_head, &s->rx_tail,
                     TCP_RX_BUF_SIZE, buf, maxlen);
    /* Ring was full: the last ACK advertised a zero window and the peer is
     * parked in its persist probe.  Push a window update now that draining
     * reopened space instead of waiting out the probe backoff. */
    if (n > 0 && free_before == 0 &&
        s->state != TCP_CLOSED && s->state != TCP_LISTEN)
        tcp_send_seg(s, TCP_ACK, 0, 0);
    return n;
}

void tcp_close(int sock)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return;
    TcpSocket *s = &g_socks[sock];
    if (s->state == TCP_ESTABLISHED || s->state == TCP_SYN_RECEIVED ||
        s->state == TCP_CLOSE_WAIT) {
        /* If a seq-carrying segment is still unacked, defer the FIN —
         * sending it now would overwrite the retx state of the in-flight
         * segment.  tcp_tick releases it once snd_una catches up. */
        if (s->snd_una != s->snd_nxt) {
            s->fin_pending = 1;
            return;
        }
        s->state = (s->state == TCP_CLOSE_WAIT) ? TCP_LAST_ACK
                                              : TCP_FIN_WAIT_1;
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

/* -------------------------------------------------------------------------
 * Retransmit helper — resend the saved segment with the original seq number.
 * We temporarily roll snd_nxt back so tcp_send_seg builds the right header,
 * then restore it.  The retransmit itself does NOT re-save retx state (that
 * would reset the counter); we update the timer manually after the call.
 * ------------------------------------------------------------------------- */
static void tcp_retransmit(TcpSocket *s)
{
    uint32_t saved_nxt = s->snd_nxt;
    s->snd_nxt = s->retx_seq;          /* rewind so header uses original seq */

    const uint8_t *payload = (s->retx_len > 0) ? s->retx_buf : 0;
    uint16_t       plen    = s->retx_len;

    /* Build and send; temporarily clear retx_count so tcp_send_seg
     * re-arms the timer (we override it right after). */
    uint8_t saved_count = s->retx_count;
    s->retx_count = 0;
    tcp_send_seg(s, s->retx_flags, payload, plen);
    s->retx_count = saved_count;       /* restore — we increment it below */

    s->snd_nxt = saved_nxt;            /* restore real snd_nxt */
}

void tcp_tick(void)
{
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        TcpSocket *s = &g_socks[i];

        switch (s->state) {

        /* ── TIME_WAIT: expire after TCP_TIMEWAIT_TICKS ─────────────────── */
        case TCP_TIME_WAIT:
            if (s->conn_timer < TCP_TIMEWAIT_TICKS)
                s->conn_timer++;
            else
                s->state = TCP_CLOSED;
            break;

        /* ── SYN_SENT: hard connect timeout ─────────────────────────────── */
        case TCP_SYN_SENT:
            s->conn_timer++;
            if (s->conn_timer >= TCP_CONN_TIMEOUT_TICKS) {
                s->state = TCP_CLOSED;   /* give up */
                break;
            }
            /* Fall through to retransmit logic for SYN retry */
            /* fall through */

        /* ── SYN_RECEIVED: half-open — reap if the peer never completes
         * the handshake (duplicate SYNs re-send SYN-ACK via tcp_rx). ── */
        case TCP_SYN_RECEIVED:
            s->conn_timer++;
            if (s->conn_timer >= TCP_CONN_TIMEOUT_TICKS)
                s->state = TCP_CLOSED;
            break;

        /* ── States with retransmittable data ───────────────────────────── */
        case TCP_ESTABLISHED:
        case TCP_FIN_WAIT_1:
        case TCP_LAST_ACK:
        case TCP_CLOSE_WAIT:
            /* A FIN deferred by tcp_close while data was in flight goes
             * out once the peer has acknowledged everything.  While
             * still waiting, fall through to normal retransmit handling
             * for the outstanding segment. */
            if (s->fin_pending && s->snd_una == s->snd_nxt &&
                (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT)) {
                s->fin_pending = 0;
                s->state = (s->state == TCP_CLOSE_WAIT) ? TCP_LAST_ACK
                                                      : TCP_FIN_WAIT_1;
                tcp_send_seg(s, TCP_FIN | TCP_ACK, 0, 0);
                break;
            }
            /* Only retransmit if there is actually unacked data/control */
            if (s->retx_timer == 0) break;       /* nothing armed */
            if (s->snd_una == s->snd_nxt) {      /* everything acked */
                s->retx_timer = 0;
                s->retx_count = 0;
                break;
            }

            s->retx_timer--;
            if (s->retx_timer > 0) break;        /* not yet */

            /* Timer expired — retransmit or abort */
            s->retx_count++;
            if (s->retx_count > TCP_RETX_MAX_TRIES) {
                /* Too many retries: send RST and close */
                tcp_send_seg(s, TCP_RST, 0, 0);
                s->state = TCP_CLOSED;
                break;
            }

            /* Retransmit the saved segment */
            tcp_retransmit(s);

            /* Exponential backoff: double the RTO, capped at max shift */
            {
                uint8_t  shift  = s->retx_count < TCP_RETX_BACKOFF_MAX
                                  ? s->retx_count : TCP_RETX_BACKOFF_MAX;
                uint16_t new_to = (uint16_t)(TCP_RETX_TICKS_INIT << shift);
                s->retx_timer   = new_to;
            }
            break;

        default:
            break;
        }
    }
}
