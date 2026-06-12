/*
 * eth.h — Ethernet II framing
 */
#ifndef UAOS_ETH_H
#define UAOS_ETH_H

#include "net.h"

#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_IP    0x0800

/* Ethernet II frame header (14 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;   /* big-endian */
} EthHdr;

#define ETH_HDR_LEN     14
#define ETH_MAX_PAYLOAD 1500
#define ETH_MAX_FRAME   (ETH_HDR_LEN + ETH_MAX_PAYLOAD)

/* Broadcast MAC */
static const uint8_t ETH_BCAST[ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/*
 * Build an Ethernet frame into buf.
 * dst/src: 6-byte MAC; ethertype in host byte order; payload already in buf+14.
 * Returns total frame length.
 */
static inline uint16_t eth_build(uint8_t *buf, const uint8_t *dst,
                                  const uint8_t *src, uint16_t ethertype,
                                  uint16_t payload_len)
{
    EthHdr *h = (EthHdr *)buf;
    net_memcpy(h->dst, dst, ETH_ALEN);
    net_memcpy(h->src, src, ETH_ALEN);
    h->ethertype = net_htons(ethertype);
    return (uint16_t)(ETH_HDR_LEN + payload_len);
}

/*
 * Dispatch an incoming Ethernet frame. Called from VirtIO-Net RX callback.
 */
void eth_rx(const uint8_t *frame, uint16_t len);

#endif /* UAOS_ETH_H */
