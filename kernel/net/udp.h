/*
 * udp.h — UDP layer
 */
#ifndef UAOS_UDP_H
#define UAOS_UDP_H

#include "net.h"

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;    /* header + data */
    uint16_t checksum;
} UdpHdr;

#define UDP_HDR_LEN 8

/* Max UDP sockets */
#define UDP_MAX_SOCKETS 8
/* Max pending receive buffer size per socket (payload bytes, shared by
 * all queued datagrams) */
#define UDP_RX_BUF_SIZE 2048
/* Max datagrams queued per socket */
#define UDP_RX_QUEUE    8

/* One queued datagram: peer + payload extent.  Payload bytes live in
 * the socket's rx_buf byte ring — datagrams occupy contiguous
 * (possibly wrapping) spans in FIFO order, so len alone bounds each. */
typedef struct {
    ipv4_t   src_ip;
    uint16_t src_port;
    uint16_t len;
} UdpDgram;

typedef struct {
    int      active;
    uint16_t local_port;
    /* Receive payload ring */
    uint8_t  rx_buf[UDP_RX_BUF_SIZE];
    uint16_t rx_head, rx_tail;
    /* Datagram queue (FIFO of pending datagrams) */
    UdpDgram rx_q[UDP_RX_QUEUE];
    uint8_t  rx_qhead, rx_qcount;
    /* Last sender info */
    ipv4_t   last_src_ip;
    uint16_t last_src_port;
} UdpSocket;

/* Handle incoming UDP datagram */
void udp_rx(ipv4_t src_ip, const uint8_t *pkt, uint16_t len);

/* Open a UDP socket bound to local_port (0 = any ephemeral) */
int  udp_open(uint16_t local_port);

/* Close a UDP socket */
void udp_close(int sock);

/* Send a UDP datagram */
int  udp_send(int sock, ipv4_t dst_ip, uint16_t dst_port,
              const uint8_t *data, uint16_t len);

/* Receive one queued UDP datagram (non-blocking).  Pops a single
 * datagram per call and reports that datagram's peer — matching BSD
 * recvfrom() semantics.  Returns the payload bytes copied, truncated
 * to maxlen with any remainder discarded, or 0 if the queue is empty
 * (or the datagram itself was zero-length). */
int  udp_recv(int sock, uint8_t *buf, uint16_t maxlen,
              ipv4_t *src_ip_out, uint16_t *src_port_out);

#endif /* UAOS_UDP_H */
