/*
 * ip.h — IPv4 layer
 */
#ifndef UAOS_IP_H
#define UAOS_IP_H

#include "net.h"

#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

/* IPv4 header (20 bytes, no options) */
typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;   /* version (4) | IHL (5) */
    uint8_t  dscp_ecn;
    uint16_t tot_len;   /* total length including header */
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} IpHdr;

#define IP_HDR_LEN  20

/* Initialise IP layer */
void ip_init(ipv4_t my_ip, ipv4_t gateway, ipv4_t netmask);

/* Handle incoming IP packet (payload after Ethernet header) */
void ip_rx(const uint8_t *pkt, uint16_t len);

/*
 * Send an IP packet.
 * dst_ip: destination IPv4 (host byte order).
 * proto:  IP_PROTO_* constant.
 * payload: already-filled buffer of payload_len bytes (placed after IP hdr).
 * Returns 1 on success.
 */
int  ip_send(ipv4_t dst_ip, uint8_t proto,
             uint8_t *payload, uint16_t payload_len);

/* Query local IP */
ipv4_t ip_get_local(void);
ipv4_t ip_get_gateway(void);
ipv4_t ip_get_netmask(void);

#endif /* UAOS_IP_H */
