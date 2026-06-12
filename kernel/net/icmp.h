/*
 * icmp.h — ICMP (ping) support
 */
#ifndef UAOS_ICMP_H
#define UAOS_ICMP_H

#include "net.h"

#define ICMP_ECHO_REQUEST   8
#define ICMP_ECHO_REPLY     0

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t ident;
    uint16_t seq;
    /* data follows */
} IcmpHdr;

#define ICMP_HDR_LEN 8

/* Handle an incoming ICMP packet */
void icmp_rx(ipv4_t src_ip, const uint8_t *pkt, uint16_t len);

/* Send an ICMP echo request (ping) to dst_ip */
void icmp_ping(ipv4_t dst_ip, uint16_t seq);

/* Returns 1 if the last ping reply was received */
int  icmp_got_reply(void);
void icmp_clear_reply(void);

#endif /* UAOS_ICMP_H */
