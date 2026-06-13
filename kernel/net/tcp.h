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

/* Retransmit tuning (tcp_tick runs at 10 Hz)
 *
 *  TCP_RETX_TICKS_INIT   — initial RTO: 10 ticks = 1 s
 *  TCP_RETX_BACKOFF_MAX  — max RTO doubling steps (1→2→4→8→16 s, then give up)
 *  TCP_RETX_MAX_TRIES    — total attempts before aborting the connection
 *  TCP_CONN_TIMEOUT_TICKS— SYN_SENT hard deadline: 75 ticks = 7.5 s
 *  TCP_TIMEWAIT_TICKS    — TIME_WAIT duration: 20 ticks = 2 s (2×MSL for QEMU)
 */
#define TCP_RETX_TICKS_INIT     10u
#define TCP_RETX_BACKOFF_MAX    4u
#define TCP_RETX_MAX_TRIES      5u
#define TCP_CONN_TIMEOUT_TICKS  75u
#define TCP_TIMEWAIT_TICKS      20u

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
    uint16_t  conn_timer;  /* general connection timer (TIME_WAIT, SYN wait) */
} TcpSocket;

/* Handle incoming TCP segment */
void tcp_rx(ipv4_t src_ip, const uint8_t *pkt, uint16_t len);

/* Open a TCP connection (active). Returns socket index or -1. */
int  tcp_connect(ipv4_t dst_ip, uint16_t dst_port, uint16_t local_port);

/* Listen on a port (passive). Returns socket index or -1. */
int  tcp_listen(uint16_t local_port);

/* Accept an incoming connection on a listening socket. Returns new socket or -1. */
int  tcp_accept(int listen_sock);

/* Send data over a TCP socket. Returns bytes queued. */
int  tcp_send(int sock, const uint8_t *data, uint16_t len);

/* Receive data from a TCP socket (non-blocking). Returns bytes read or 0. */
int  tcp_recv(int sock, uint8_t *buf, uint16_t maxlen);

/* Close a TCP socket (sends FIN). */
void tcp_close(int sock);

/* Query socket state */
TcpState tcp_state(int sock);

/* Must be called periodically (e.g. from PIT tick) for retransmit/timeout */
void tcp_tick(void);

#endif /* UAOS_TCP_H */
