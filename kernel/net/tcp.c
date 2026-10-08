/*
 * tcp.c — TCP state machine
 *
 * Implements a minimal but correct TCP stack supporting:
 *   - Active connect (SYN → ESTABLISHED → data → FIN)
 *   - Passive listen/accept
 *   - Data send/receive with ACK
 *   - Connection teardown (FIN/FIN-ACK)
 *   - Retransmit timer with exponential backoff (tcp_tick, from the 100 Hz PIT)
 *   - Connect timeout (SYN_SENT), TIME_WAIT expiry, half-open cleanup,
 *     CLOSE_WAIT linger bound, FIN_WAIT_2 wait bound
 *   - Peer-window enforcement and a bounded in-flight segment count on send
 *   - RFC 793 RST generation for segments that match no socket
 *
 * Send window (no Nagle): up to TCP_RETX_SLOTS seq-carrying segments may
 * be in flight per socket, bounded further by the peer's advertised
 * window.  Every such segment is kept in the retx queue until a
 * cumulative ACK covers it; RTO expiry replays the oldest entry.
 */
#include "tcp.h"
#include "ip.h"
#include "../irq/irq.h"

static TcpSocket g_socks[TCP_MAX_SOCKETS];
static uint32_t  g_isn_counter = 0x12345678;  /* initial seq number seed */

/* Connection-generation counter — stamped into every freshly allocated
 * socket so a saved (index, gen) pair identifies one connection even
 * after the slot is retired and reissued (UAOS-263).  Never 0. */
static uint32_t  g_conn_gen_next = 1;

static uint32_t next_conn_gen(void)
{
    uint32_t g = g_conn_gen_next++;
    if (!g_conn_gen_next) g_conn_gen_next = 1;
    return g;
}

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
 * Emit a TCP segment with an explicit sequence number.  Does not touch
 * snd_nxt or the retransmit queue — used both for fresh sends (via
 * tcp_send_seg) and for retransmission replay (via tcp_retransmit).
 * ------------------------------------------------------------------------- */
static void tcp_emit(TcpSocket *s, uint32_t seq, uint8_t flags,
                     const uint8_t *data, uint16_t data_len)
{
    uint8_t seg[TCP_HDR_LEN + TCP_MSS];
    if (data_len > TCP_MSS) data_len = TCP_MSS;
    uint16_t seg_len = (uint16_t)(TCP_HDR_LEN + data_len);

    TcpHdr *h = (TcpHdr *)seg;
    h->src_port  = net_htons(s->local_port);
    h->dst_port  = net_htons(s->remote_port);
    h->seq       = net_htonl(seq);
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
    ip_send(s->remote_ip, IP_PROTO_TCP, seg, seg_len);
}

/* -------------------------------------------------------------------------
 * Send a new TCP segment at snd_nxt, advancing it, and queue a copy for
 * retransmit if it carries sequence space.  Callers guarantee a free
 * retx slot: tcp_send checks retx_nseg, and SYN/FIN sends happen only
 * with an empty queue (socket alloc, deferred-FIN release).
 * ------------------------------------------------------------------------- */
static void tcp_send_seg(TcpSocket *s, uint8_t flags,
                          const uint8_t *data, uint16_t data_len)
{
    if (data_len > TCP_MSS) data_len = TCP_MSS;

    /* Record snd_nxt BEFORE advancing, for the queue entry */
    uint32_t seq_before = s->snd_nxt;

    tcp_emit(s, seq_before, flags, data, data_len);

    /* Advance snd_nxt for data and SYN/FIN (each consumes 1 seq) */
    if (flags & (TCP_SYN | TCP_FIN)) s->snd_nxt++;
    s->snd_nxt += data_len;

    /* Queue segment for retransmit — but not pure ACKs (nothing to replay) */
    if ((flags & (TCP_SYN | TCP_FIN)) || data_len > 0) {
        uint8_t idx = (uint8_t)((s->retx_head + s->retx_nseg) % TCP_RETX_SLOTS);
        TcpRetxSeg *r = &s->retx_q[idx];
        r->seq   = seq_before;
        r->flags = flags;
        r->len   = 0;
        if (data && data_len) {
            net_memcpy(r->data, data, data_len);
            r->len = data_len;
        }
        s->retx_nseg++;
        /* Arm the retransmit timer only when it is stopped — the RTO
         * belongs to the oldest unacked segment, so a send while others
         * are in flight does not restart it.  retx_count is NOT reset
         * here — that happens only when snd_una advances. */
        if (s->retx_timer == 0)
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
    /* A SYN carrying the tuple of a socket still in TIME_WAIT is a fast
     * reconnect, not a stray retransmit: retire the 2MSL remnant and let
     * the listener path below spawn a fresh connection instead of
     * silently swallowing the SYN for the remnant's remaining wait. */
    if (s && (flags & TCP_SYN) && s->state == TCP_TIME_WAIT) {
        s->state = TCP_CLOSED;
        s = 0;
    }
    if (!s) {
        /* Check for listener */
        if (flags & TCP_SYN) {
            s = find_sock(src_ip, src_port, dst_port, 1);
            if (s) {
                /* Spawn new socket for this connection */
                TcpSocket *ns = alloc_sock();
                if (ns) {
                    net_memset(ns, 0, sizeof(*ns));
                    ns->conn_gen    = next_conn_gen();
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
                    /* The SYN-ACK stays in flight (snd_una < snd_nxt) so
                     * tcp_tick retransmits it on RTO expiry — a peer whose
                     * handshake-completing ACK was lost would otherwise pin
                     * the half-open until the conn_timer reap (UAOS-260). */
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
            s->snd_una = ack_num;
            /* Retire queue entries fully covered by this cumulative ACK.
             * A segment straddling ack_num is only partially delivered —
             * it stays at the head for retransmit. */
            while (s->retx_nseg) {
                TcpRetxSeg *r = &s->retx_q[s->retx_head];
                uint32_t end = r->seq + r->len +
                               ((r->flags & TCP_SYN) ? 1u : 0u) +
                               ((r->flags & TCP_FIN) ? 1u : 0u);
                if (seq_gt(end, ack_num)) break;
                s->retx_head = (uint8_t)((s->retx_head + 1) % TCP_RETX_SLOTS);
                s->retx_nseg--;
            }
            if (s->snd_una == s->snd_nxt) {
                /* Everything in flight acked — disarm. */
                s->retx_timer = 0;
                s->retx_count = 0;
            } else {
                /* Partial ACK: progress, but the tail is still unacked.
                 * Restart the RTO for the new oldest segment — clearing
                 * the timer here left snd_una < snd_nxt forever, which
                 * wedged fin_pending sockets in CLOSE_WAIT (UAOS-223). */
                s->retx_timer = TCP_RETX_TICKS_INIT;
                s->retx_count = 0;
            }
        }
        s->snd_wnd = net_ntohs(h->window);
    }

    /* A RST tears the connection down in every synchronized state —
     * only SYN_SENT/ESTABLISHED used to honor it, so a force-closing
     * peer (RST/RST|ACK in CLOSE_WAIT, LAST_ACK, FIN_WAIT_x, ...) was
     * ignored and the slot leaked (UAOS-223). */
    if (flags & TCP_RST) {
        s->state = TCP_CLOSED;
        return;
    }

    switch (s->state) {

    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            s->rcv_nxt = seq + 1;
            s->state   = TCP_ESTABLISHED;
            tcp_send_seg(s, TCP_ACK, 0, 0);
        }
        break;

    case TCP_SYN_RECEIVED:
        /* The completing ACK must cover our SYN-ACK (ack == snd_nxt);
         * a bare or out-of-window ACK must not promote a still-unacked
         * half-open into ESTABLISHED. */
        if ((flags & TCP_ACK) && ack_num == s->snd_nxt) {
            s->state = TCP_ESTABLISHED;
        } else if (flags & TCP_SYN) {
            /* Duplicate SYN — our SYN-ACK was lost (e.g. dropped while
             * ARP resolved).  Replay the saved segment so the handshake
             * can still complete. */
            tcp_retransmit(s);
        }
        break;

    case TCP_ESTABLISHED:
        /* Queue received data; the FIN only counts once every byte
         * before it has actually been delivered to the ring. */
        if (data_len > 0)
            tcp_rx_data(s, seq, data, data_len);
        if ((flags & TCP_FIN) && seq + data_len == s->rcv_nxt) {
            s->rcv_nxt++;
            s->state      = TCP_CLOSE_WAIT;
            s->conn_timer = 0;   /* start the linger bound (tcp_tick) */
            tcp_send_seg(s, TCP_ACK, 0, 0);
        }
        break;

    case TCP_FIN_WAIT_1:
        if (data_len > 0)
            tcp_rx_data(s, seq, data, data_len);
        /* Only an ACK covering our FIN (snd_nxt — the FIN consumed one
         * sequence number) may advance us to FIN_WAIT_2; a dup ACK must
         * leave us here where the retransmit machinery still owns the
         * outstanding FIN. */
        if ((flags & TCP_ACK) && ack_num == s->snd_nxt) {
            s->state      = TCP_FIN_WAIT_2;
            s->conn_timer = 0;   /* start the wait-for-peer-FIN bound */
        }
        if ((flags & TCP_FIN) && seq + data_len == s->rcv_nxt) {
            s->rcv_nxt++;
            tcp_send_seg(s, TCP_ACK, 0, 0);
            s->state      = TCP_TIME_WAIT;
            s->conn_timer = 0;
        }
        break;

    case TCP_FIN_WAIT_2:
        if (data_len > 0) {
            tcp_rx_data(s, seq, data, data_len);
            s->conn_timer = 0;   /* peer still talking — extend wait */
        }
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
        /* Only an ACK covering our FIN ends the connection; a stale
         * dup must not close it early (the FIN's retransmits still
         * bound the wait via tcp_tick). */
        if ((flags & TCP_ACK) && ack_num == s->snd_nxt)
            s->state = TCP_CLOSED;
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

/* Ephemeral source port for outbound connections.  Rotates through the
 * IANA dynamic range so a rapid reconnect to the same peer gets a fresh
 * 4-tuple — the guest's TIME_WAIT (2 s) is far shorter than a typical
 * host's (~60 s), and reusing a slot-derived port made every successive
 * connect to the same server share the old connection's tuple, which
 * the still-TIME_WAIT peer answers with RST. */
static uint16_t g_eph_next = 49152;

static uint16_t pick_ephemeral_port(void)
{
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t cand = g_eph_next;
        if (++g_eph_next == 0 || g_eph_next < 49152)
            g_eph_next = 49152;
        int inuse = 0;
        for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
            if (g_socks[i].state != TCP_CLOSED &&
                g_socks[i].local_port == cand) {
                inuse = 1;
                break;
            }
        }
        if (!inuse)
            return cand;
    }
    return g_eph_next;
}

int tcp_connect(ipv4_t dst_ip, uint16_t dst_port, uint16_t local_port)
{
    TcpSocket *s = alloc_sock();
    if (!s) return -1;
    net_memset(s, 0, sizeof(*s));
    s->conn_gen    = next_conn_gen();
    s->state       = TCP_SYN_SENT;
    s->local_ip    = ip_get_local();
    s->local_port  = local_port ? local_port : pick_ephemeral_port();
    s->remote_ip   = dst_ip;
    s->remote_port = dst_port;
    s->accepted    = 1;   /* outbound socket — never a pending accept      */
    s->snd_nxt     = g_isn_counter;
    s->snd_una     = g_isn_counter;
    g_isn_counter += 0x10000;
    tcp_send_seg(s, TCP_SYN, 0, 0);
    return sock_idx(s);
}

int tcp_listen(uint16_t local_port)
{
    /* Reject a second LISTEN on the same port — otherwise a double-start
     * (e.g. telnetd from User-Startup + manual) leaves two accept loops
     * racing over the same backlog.  The scan and the claim must be
     * atomic: two concurrent callers can otherwise both pass the scan. */
    uint64_t fl = irq_save();
    for (int i = 0; i < TCP_MAX_SOCKETS; i++)
        if (g_socks[i].state == TCP_LISTEN &&
            g_socks[i].local_port == local_port) {
            irq_restore(fl);
            return -1;
        }
    TcpSocket *s = alloc_sock();
    if (!s) { irq_restore(fl); return -1; }
    net_memset(s, 0, sizeof(*s));
    s->conn_gen   = next_conn_gen();
    s->state      = TCP_LISTEN;
    s->local_ip   = ip_get_local();
    s->local_port = local_port;
    int idx = sock_idx(s);
    irq_restore(fl);
    return idx;
}

int tcp_accept(int listen_sock, uint32_t *gen_out)
{
    if (listen_sock < 0 || listen_sock >= TCP_MAX_SOCKETS) return -1;
    if (g_socks[listen_sock].state != TCP_LISTEN) return -1;
    /* Return the first connection that completed its handshake and has
     * not been claimed yet.  Without the accepted mark an active session
     * socket — also ESTABLISHED on this port — would be returned again.
     * CLOSE_WAIT is returned too: a peer that FINs between handshake and
     * accept must still be handed to a reader (which will drain the ring
     * and close) instead of orphaning the slot (UAOS-223).  The claim and
     * the conn_gen read happen under one irq_save: tcp_rx could otherwise
     * retire the socket and reissue the slot between the two (UAOS-263). */
    uint64_t fl = irq_save();
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if ((g_socks[i].state == TCP_ESTABLISHED ||
             g_socks[i].state == TCP_CLOSE_WAIT) &&
            !g_socks[i].accepted &&
            g_socks[i].local_port == g_socks[listen_sock].local_port) {
            g_socks[i].accepted = 1;
            if (gen_out) *gen_out = g_socks[i].conn_gen;
            irq_restore(fl);
            return i;
        }
    }
    irq_restore(fl);
    return -1;
}

int tcp_send(int sock, const uint8_t *data, uint16_t len)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return 0;
    TcpSocket *s = &g_socks[sock];
    if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT) return 0;
    if (len == 0 || s->fin_pending) return 0;   /* close already requested */

    /* Bounded in-flight: the retx queue must have a free slot, and
     * unacked bytes must stay inside the peer's advertised window.
     * Return 0 and let the caller poll/retry when either bound is hit. */
    if (s->retx_nseg >= TCP_RETX_SLOTS) return 0;
    uint32_t in_flight = s->snd_nxt - s->snd_una;
    if (in_flight >= s->snd_wnd) return 0;
    uint32_t avail = s->snd_wnd - in_flight;
    if (len > avail)   len = (uint16_t)avail;
    if (len > TCP_MSS) len = TCP_MSS;

    tcp_send_seg(s, TCP_PSH | TCP_ACK, data, len);
    if (s->state == TCP_CLOSE_WAIT)
        s->conn_timer = 0;   /* owner still working — extend linger */
    return len;
}

int tcp_recv(int sock, uint8_t *buf, uint16_t maxlen)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return 0;
    TcpSocket *s = &g_socks[sock];
    uint16_t free_before = rbuf_free(s->rx_head, s->rx_tail, TCP_RX_BUF_SIZE);
    int n = rbuf_get(s->rx_buf, &s->rx_head, &s->rx_tail,
                     TCP_RX_BUF_SIZE, buf, maxlen);
    if (n > 0 && s->state == TCP_CLOSE_WAIT)
        s->conn_timer = 0;   /* owner still draining — extend linger */
    /* Draining a non-empty ring freed space the peer's stream was
     * probably blocked on: every ACK we sent while queuing advertised a
     * shrinking window, so the sender parks in zero-window persist mode
     * (multi-second probes) once it has filled what we last advertised.
     * Announce the reopened window now instead of waiting out the probe
     * backoff. */
    if (n > 0 && free_before < TCP_RX_BUF_SIZE &&
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

void tcp_abort(int sock)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return;
    g_socks[sock].state = TCP_CLOSED;
}

TcpState tcp_state(int sock)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return TCP_CLOSED;
    return g_socks[sock].state;
}

/* -------------------------------------------------------------------------
 * Connection-generation checked operations (UAOS-263)
 *
 * A bare socket index does not identify a connection: tcp_rx (NIC IRQ or
 * another task's net_stack_poll) and tcp_tick (PIT) can retire it — RST,
 * retransmit exhaustion, teardown bounds — and the slot can be reissued
 * to a different connection while the old owner is still holding the
 * index.  Each wrapper verifies the caller's generation under irq_save
 * (which also blocks the PIT tick and NIC IRQ, making check+op atomic on
 * this single-CPU kernel) and refuses to act on a recycled slot.
 * ------------------------------------------------------------------------- */
static int conn_matches(int sock, uint32_t gen)
{
    return sock >= 0 && sock < TCP_MAX_SOCKETS &&
           g_socks[sock].state != TCP_CLOSED &&
           g_socks[sock].conn_gen == gen;
}

uint32_t tcp_conn_gen(int sock)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return 0;
    return g_socks[sock].conn_gen;
}

TcpState tcp_conn_state(int sock, uint32_t gen)
{
    uint64_t fl = irq_save();
    TcpState st = conn_matches(sock, gen) ? g_socks[sock].state
                                        : TCP_CLOSED;
    irq_restore(fl);
    return st;
}

int tcp_conn_send(int sock, uint32_t gen, const uint8_t *data, uint16_t len)
{
    uint64_t fl = irq_save();
    int n = conn_matches(sock, gen) ? tcp_send(sock, data, len) : 0;
    irq_restore(fl);
    return n;
}

int tcp_conn_recv(int sock, uint32_t gen, uint8_t *buf, uint16_t maxlen)
{
    uint64_t fl = irq_save();
    int n = conn_matches(sock, gen) ? tcp_recv(sock, buf, maxlen) : 0;
    irq_restore(fl);
    return n;
}

void tcp_conn_close(int sock, uint32_t gen)
{
    uint64_t fl = irq_save();
    if (conn_matches(sock, gen)) {
        TcpState st = g_socks[sock].state;
        /* Drive a graceful close only from the live states.  A socket
         * already in FIN_WAIT/LAST_ACK/TIME_WAIT is being torn down —
         * either by us earlier or by the connection's other owner — and
         * tcp_tick's bounds retire it; stomping it with a second close
         * would hard-drop a teardown in flight. */
        if (st == TCP_SYN_RECEIVED || st == TCP_ESTABLISHED ||
            st == TCP_CLOSE_WAIT)
            tcp_close(sock);
    }
    irq_restore(fl);
}

void tcp_conn_abort(int sock, uint32_t gen)
{
    uint64_t fl = irq_save();
    if (conn_matches(sock, gen))
        g_socks[sock].state = TCP_CLOSED;
    irq_restore(fl);
}

int tcp_peer(int sock, ipv4_t *ip, uint16_t *port)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return -1;
    if (ip)   *ip   = g_socks[sock].remote_ip;
    if (port) *port = g_socks[sock].remote_port;
    return 0;
}

/* -------------------------------------------------------------------------
 * Retransmit helper — replay the oldest unacked segment with its original
 * seq number.  tcp_emit sends raw: snd_nxt is untouched and the segment is
 * already queued, so no retx state changes here.  (tcp_rx also uses this
 * to replay a lost SYN-ACK when a duplicate SYN arrives in SYN_RECEIVED.)
 * ------------------------------------------------------------------------- */
static void tcp_retransmit(TcpSocket *s)
{
    if (s->retx_nseg == 0) return;
    const TcpRetxSeg *r = &s->retx_q[s->retx_head];
    tcp_emit(s, r->seq, r->flags, r->len ? r->data : 0, r->len);
}

/* -------------------------------------------------------------------------
 * Shared per-tick retransmit handling — runs in every state that can hold
 * an unacked seq-carrying segment: SYN in SYN_SENT, SYN-ACK in
 * SYN_RECEIVED, data/FIN in ESTABLISHED and the teardown states.  Gives up
 * (RST + CLOSED) after TCP_RETX_MAX_TRIES so no half-dead state can pin a
 * socket slot forever.
 * ------------------------------------------------------------------------- */
static void tcp_tick_retx(TcpSocket *s)
{
    /* A FIN deferred by tcp_close while data was in flight goes out once
     * the peer has acknowledged everything. */
    if (s->fin_pending && s->snd_una == s->snd_nxt &&
        (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT)) {
        s->fin_pending = 0;
        s->state = (s->state == TCP_CLOSE_WAIT) ? TCP_LAST_ACK
                                              : TCP_FIN_WAIT_1;
        tcp_send_seg(s, TCP_FIN | TCP_ACK, 0, 0);
        return;
    }
    if (s->snd_una == s->snd_nxt) {      /* everything acked */
        s->retx_timer = 0;
        s->retx_count = 0;
        return;
    }
    /* Unacked data with a disarmed timer is a wedge: re-arm so the
     * segment retransmits (or aborts after TCP_RETX_MAX_TRIES) — a
     * deferred FIN can never pin the slot again (UAOS-223). */
    if (s->retx_timer == 0)
        s->retx_timer = TCP_RETX_TICKS_INIT;

    s->retx_timer--;
    if (s->retx_timer > 0) return;       /* not yet */

    /* Timer expired — retransmit or abort */
    s->retx_count++;
    if (s->retx_count > TCP_RETX_MAX_TRIES) {
        /* Too many retries: send RST and close */
        tcp_send_seg(s, TCP_RST, 0, 0);
        s->state = TCP_CLOSED;
        return;
    }

    /* Retransmit the oldest unacked segment */
    tcp_retransmit(s);

    /* Exponential backoff: double the RTO, capped at max shift */
    {
        uint8_t  shift  = s->retx_count < TCP_RETX_BACKOFF_MAX
                          ? s->retx_count : TCP_RETX_BACKOFF_MAX;
        uint16_t new_to = (uint16_t)(TCP_RETX_TICKS_INIT << shift);
        s->retx_timer   = new_to;
    }
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

        /* ── SYN_SENT / SYN_RECEIVED: bounded handshake ───────────────────
         * conn_timer is the hard half-open deadline, counted ONCE per tick
         * (the old fall-through double-counted it, halving the budget —
         * UAOS-260).  The SYN / SYN-ACK ride the shared retransmit path so
         * a single lost handshake segment no longer fails the connect —
         * the retx abort (~4.7 s) normally wins before this deadline. */
        case TCP_SYN_SENT:
        case TCP_SYN_RECEIVED:
            s->conn_timer++;
            if (s->conn_timer >= TCP_CONN_TIMEOUT_TICKS) {
                s->state = TCP_CLOSED;   /* give up */
                break;
            }
            tcp_tick_retx(s);
            break;

        /* ── FIN_WAIT_2: bound the wait for the peer's FIN ──────────────── */
        case TCP_FIN_WAIT_2:
            /* Our FIN is acked; a peer that never closes (dead app,
             * deliberate half-open) would pin the slot forever.  Peer
             * data refreshes conn_timer in tcp_rx. */
            s->conn_timer++;
            if (s->conn_timer >= TCP_FINWAIT2_TICKS)
                s->state = TCP_CLOSED;
            break;

        /* ── CLOSE_WAIT: bound the linger, then shared retx handling ────── */
        case TCP_CLOSE_WAIT:
            /* The peer already sent FIN; a live owner drains the ring
             * and closes promptly.  Owner activity (tcp_send/tcp_recv
             * progress) resets conn_timer, so reaching the bound means
             * the socket is abandoned — take over the close ourselves
             * instead of leaking the slot (UAOS-223).  If data is still
             * in flight tcp_close defers via fin_pending and the shared
             * retransmit path below bounds the rest. */
            s->conn_timer++;
            if (s->conn_timer >= TCP_CLOSEWAIT_TICKS) {
                s->conn_timer = 0;
                tcp_close(i);
            }
            /* fall through */

        /* ── States with retransmittable data ───────────────────────────── */
        case TCP_ESTABLISHED:
        case TCP_FIN_WAIT_1:
        case TCP_LAST_ACK:
            tcp_tick_retx(s);
            break;

        default:
            break;
        }
    }
}

/* -------------------------------------------------------------------------
 * C:netstat — TCP socket table dump (UAOS-203).  Read-only.
 * ------------------------------------------------------------------------- */
#include "../dbg/diag.h"

static void d_ip(DiagLine *l, ipv4_t ip)
{
    dl_dec(l, (ip >> 24) & 0xFF); dl_ch(l, '.');
    dl_dec(l, (ip >> 16) & 0xFF); dl_ch(l, '.');
    dl_dec(l, (ip >> 8) & 0xFF);  dl_ch(l, '.');
    dl_dec(l, ip & 0xFF);
}

static const char *d_tcp_state(TcpState s)
{
    static const char *const names[] = {
        "closed","syn-sent","syn-recv","estab","fin-w1","fin-w2",
        "close-w","last-ack","time-w","listen"
    };
    return (s >= 0 && s <= TCP_LISTEN) ? names[s] : "?";
}

void Tcp_DiagDump(void *ctx, void (*emit)(void *, const char *))
{
    DiagLine l;
    int open = 0;
    emit(ctx, " tcp# state    local              remote             rx   tx  retx");
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        TcpSocket *s = &g_socks[i];
        if (s->state == TCP_CLOSED) continue;
        open++;
        dl_reset(&l);
        dl_ch(&l, ' '); dl_dec(&l, (uint64_t)i); dl_pad(&l, 5);
        dl_add(&l, d_tcp_state(s->state)); dl_pad(&l, 13);
        d_ip(&l, s->local_ip); dl_ch(&l, ':'); dl_dec(&l, s->local_port);
        dl_pad(&l, 33);
        d_ip(&l, s->remote_ip); dl_ch(&l, ':'); dl_dec(&l, s->remote_port);
        dl_pad(&l, 53);
        uint32_t rxq = (uint16_t)(s->rx_head - s->rx_tail);
        uint32_t txq = s->snd_nxt - s->snd_una;   /* bytes in flight */
        dl_dec(&l, rxq); dl_pad(&l, 58);
        dl_dec(&l, txq); dl_pad(&l, 63);
        dl_dec(&l, s->retx_count);
        dl_emit(&l, ctx, emit);
    }
    dl_reset(&l);
    dl_add(&l, " "); dl_dec(&l, (uint64_t)open); dl_add(&l, " open TCP socket(s)");
    dl_emit(&l, ctx, emit);
}
