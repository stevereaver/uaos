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
/* Max pending receive buffer size per socket */
#define UDP_RX_BUF_SIZE 2048

typedef struct {
    int      active;
    uint16_t local_port;
    /* Receive ring buffer */
    uint8_t  rx_buf[UDP_RX_BUF_SIZE];
    uint16_t rx_head, rx_tail;
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

/* Receive a UDP datagram (non-blocking). Returns bytes read or 0. */
int  udp_recv(int sock, uint8_t *buf, uint16_t maxlen,
              ipv4_t *src_ip_out, uint16_t *src_port_out);

#endif /* UAOS_UDP_H */
