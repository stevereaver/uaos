/*
 * arp.h — ARP (Address Resolution Protocol) for IPv4/Ethernet
 */
#ifndef UAOS_ARP_H
#define UAOS_ARP_H

#include "net.h"

/* ARP cache size */
#define ARP_CACHE_SIZE  16

/* ARP packet (28 bytes for IPv4/Ethernet) */
typedef struct __attribute__((packed)) {
    uint16_t htype;     /* hardware type: 1 = Ethernet */
    uint16_t ptype;     /* protocol type: 0x0800 = IPv4 */
    uint8_t  hlen;      /* hardware addr len: 6 */
    uint8_t  plen;      /* protocol addr len: 4 */
    uint16_t oper;      /* 1=request, 2=reply */
    uint8_t  sha[ETH_ALEN]; /* sender hardware addr */
    uint32_t spa;           /* sender protocol addr */
    uint8_t  tha[ETH_ALEN]; /* target hardware addr */
    uint32_t tpa;           /* target protocol addr */
} ArpPkt;

/* Initialise the ARP module with our IP + MAC */
void arp_init(ipv4_t my_ip, const uint8_t *my_mac);

/* Handle an incoming ARP packet (payload after Ethernet header) */
void arp_rx(const uint8_t *pkt, uint16_t len);

/*
 * Look up a MAC for a given IP.  Returns 1 and fills mac_out if found.
 * Returns 0 if not in cache (caller should send an ARP request and retry).
 */
int  arp_lookup(ipv4_t ip, uint8_t *mac_out);

/* Send an ARP request for the given IP */
void arp_request(ipv4_t target_ip);

/* Add/update an entry in the ARP cache */
void arp_cache_update(ipv4_t ip, const uint8_t *mac);

#endif /* UAOS_ARP_H */
