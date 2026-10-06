/*
 * tcp.h — TCP layer (client + passive server)
 */
#ifndef UAOS_TCP_H
#define UAOS_TCP_H

#include "net.h"

/* TCP header (20 bytes, no options) */
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;  /* (header_len/4) << 4 */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} TcpHdr;

#define TCP_HDR_LEN     20

/* TCP flags */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10
#define TCP_URG  0x20

/* TCP connection states */
typedef enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
    TCP_LISTEN
} TcpState;

/* Max TCP sockets */
#define TCP_MAX_SOCKETS     8
#define TCP_TX_BUF_SIZE     4096
#define TCP_RX_BUF_SIZE     4096

/* Max payload per segment (Ethernet MTU 1500 - IP hdr 20 - TCP hdr 20) */
#define TCP_MSS             1460

/* Retransmit / timeout tuning (tcp_tick runs at the 100 Hz PIT rate)
 *
 *  TCP_RETX_TICKS_INIT   — initial RTO: 10 ticks = 100 ms
 *  TCP_RETX_BACKOFF_MAX  — max RTO doubling steps (100→200→...→1600 ms, then give up)
 *  TCP_RETX_MAX_TRIES    — total attempts before aborting the connection
 *  TCP_CONN_TIMEOUT_TICKS— SYN_SENT/SYN_RECEIVED hard deadline: 500 ticks =
 *                          5 s; a backstop only — handshake retransmits abort
 *                          on their own at ~4.7 s via TCP_RETX_MAX_TRIES
 *  TCP_TIMEWAIT_TICKS    — TIME_WAIT duration: 20 ticks = 200 ms (QEMU LAN)
 *  TCP_CLOSEWAIT_TICKS   — CLOSE_WAIT idle bound: 3000 ticks = 30 s; an
 *                          owner that never closes loses the slot to the
 *                          stack, which drives the close itself (UAOS-223)
 *  TCP_FINWAIT2_TICKS    — wait for the peer's FIN after ours was acked:
 *                          12000 ticks = 120 s (peer data refreshes it)
 */
#define TCP_RETX_TICKS_INIT     10u
#define TCP_RETX_BACKOFF_MAX    4u
#define TCP_RETX_MAX_TRIES      5u
#define TCP_CONN_TIMEOUT_TICKS  500u
#define TCP_TIMEWAIT_TICKS      20u
#define TCP_CLOSEWAIT_TICKS     3000u
#define TCP_FINWAIT2_TICKS      12000u

/* Retransmit buffer: holds the payload of the last sent-but-unacked segment.
 * We only need one outstanding segment (single-segment send model). */
#define TCP_RETX_BUF_SIZE  1460

typedef struct {
    TcpState  state;
    ipv4_t    local_ip;
    uint16_t  local_port;
    ipv4_t    remote_ip;
    uint16_t  remote_port;
    uint32_t  snd_nxt;      /* next sequence number to send */
    uint32_t  snd_una;      /* oldest unacknowledged seq */
    uint32_t  rcv_nxt;      /* next expected from remote */
    uint16_t  snd_wnd;      /* remote receive window */
    /* TX buffer (unsent or unacked data) */
    uint8_t   tx_buf[TCP_TX_BUF_SIZE];
    uint16_t  tx_head, tx_tail;
    /* RX buffer (received data ready for app) */
    uint8_t   rx_buf[TCP_RX_BUF_SIZE];
    uint16_t  rx_head, rx_tail;
    /* Retransmit state */
    uint8_t   retx_buf[TCP_RETX_BUF_SIZE]; /* copy of last sent payload     */
    uint16_t  retx_len;    /* length of retx_buf (0 = nothing pending)       */
    uint8_t   retx_flags;  /* TCP flags of the last sent segment             */
    uint32_t  retx_seq;    /* snd_nxt at the time the segment was sent       */
    uint16_t  retx_timer;  /* ticks until next retransmit (counts down)      */
    uint8_t   retx_count;  /* number of retransmits already attempted        */
    uint8_t   fin_pending; /* tcp_close deferred while data is unacked       */
    uint8_t   accepted;    /* claimed by tcp_accept — never handed out twice */
    uint16_t  conn_timer;  /* general connection timer (SYN wait, TIME_WAIT,
                            * CLOSE_WAIT linger, FIN_WAIT_2 wait-for-FIN) */
    uint32_t  conn_gen;    /* connection-generation token, stamped at alloc —
                            * a saved (sock, gen) pair proves the slot still
                            * holds the same connection (UAOS-263)          */
} TcpSocket;

/* Handle incoming TCP segment */
void tcp_rx(ipv4_t src_ip, ipv4_t dst_ip, const uint8_t *pkt, uint16_t len);

/* Open a TCP connection (active). Returns socket index or -1. */
int  tcp_connect(ipv4_t dst_ip, uint16_t dst_port, uint16_t local_port);

/* Listen on a port (passive). Returns socket index or -1. */
int  tcp_listen(uint16_t local_port);

/* Accept an incoming connection on a listening socket.  Returns the new
 * socket index or -1; when gen_out is non-NULL it receives the accepted
 * connection's generation token — the mark and the read are done under
 * one irq_save so the token provably belongs to the returned socket
 * (the slot could be RST-freed and reallocated by tcp_rx before a
 * separate tcp_conn_gen() call ran — UAOS-263). */
int  tcp_accept(int listen_sock, uint32_t *gen_out);

/* Send data over a TCP socket.  Returns bytes sent; 0 when the socket is
 * busy (a segment is still unacked) or the peer window is closed — the
 * caller should poll the stack and retry. */
int  tcp_send(int sock, const uint8_t *data, uint16_t len);

/* Receive data from a TCP socket (non-blocking). Returns bytes read or 0. */
int  tcp_recv(int sock, uint8_t *buf, uint16_t maxlen);

/* Close a TCP socket (sends FIN). */
void tcp_close(int sock);

/* Abort a TCP socket without transmitting anything.  For teardown after
 * the stack has been shut down, where a FIN could never be answered and
 * tcp_tick no longer runs to retire the closing socket. */
void tcp_abort(int sock);

/* Query socket state */
TcpState tcp_state(int sock);

/* Connection-generation query + checked operations (UAOS-263).
 *
 * tcp_rx (NIC IRQ / net_stack_poll) and tcp_tick (PIT) can retire a
 * socket — RST, retransmit exhaustion, teardown timers — while a task
 * still holds its index, and the freed slot can be reissued to a new
 * connection before the owner notices.  A bare index therefore does not
 * identify a connection; the conn_gen token does.  Each tcp_conn_* call
 * verifies the generation under irq_save and performs the operation only
 * when the slot still holds the caller's connection — a stale owner can
 * no longer read from, write to, or close a different connection that
 * recycled the slot.
 *
 * tcp_conn_close() only drives a graceful close from the live states
 * (SYN_RECEIVED, ESTABLISHED, CLOSE_WAIT); a connection already in
 * FIN_WAIT/LAST_ACK/TIME_WAIT teardown is left for tcp_tick's bounds —
 * a second closer must not abort a teardown already in flight. */
uint32_t tcp_conn_gen(int sock);
TcpState tcp_conn_state(int sock, uint32_t gen);
int  tcp_conn_send(int sock, uint32_t gen, const uint8_t *data, uint16_t len);
int  tcp_conn_recv(int sock, uint32_t gen, uint8_t *buf, uint16_t maxlen);
void tcp_conn_close(int sock, uint32_t gen);
void tcp_conn_abort(int sock, uint32_t gen);

/* Read the peer address/port of a socket (for connection logging and
 * session listings).  Returns 0 on success, -1 for a bad index. */
int  tcp_peer(int sock, ipv4_t *ip, uint16_t *port);

/* Must be called periodically (e.g. from PIT tick) for retransmit/timeout */
void tcp_tick(void);

#endif /* UAOS_TCP_H */
