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
