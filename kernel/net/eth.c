/*
 * eth.c — Ethernet II RX dispatch
 */
#include "eth.h"
#include "arp.h"
#include "ip.h"

void eth_rx(const uint8_t *frame, uint16_t len)
{
    if (len < ETH_HDR_LEN) return;
    const EthHdr *h = (const EthHdr *)frame;
    uint16_t et = net_ntohs(h->ethertype);
    const uint8_t *payload = frame + ETH_HDR_LEN;
    uint16_t plen = (uint16_t)(len - ETH_HDR_LEN);

    switch (et) {
    case ETHERTYPE_ARP:
        arp_rx(payload, plen);
        break;
    case ETHERTYPE_IP:
        ip_rx(payload, plen);
        break;
    default:
        break;
    }
}
