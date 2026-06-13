/*
 * arp.c — ARP implementation
 */
#include "arp.h"
#include "eth.h"
#include "net_device.h"

static ipv4_t   g_my_ip  = 0;
static uint8_t  g_my_mac[ETH_ALEN];

/* ARP cache */
typedef struct { ipv4_t ip; uint8_t mac[ETH_ALEN]; uint8_t valid; } ArpEntry;
static ArpEntry g_cache[ARP_CACHE_SIZE];

void arp_init(ipv4_t my_ip, const uint8_t *my_mac)
{
    g_my_ip = my_ip;
    net_memcpy(g_my_mac, my_mac, ETH_ALEN);
    net_memset(g_cache, 0, sizeof(g_cache));
}

void arp_cache_update(ipv4_t ip, const uint8_t *mac)
{
    /* Update existing entry */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].valid && g_cache[i].ip == ip) {
            net_memcpy(g_cache[i].mac, mac, ETH_ALEN);
            return;
        }
    }
    /* Find empty slot */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!g_cache[i].valid) {
            g_cache[i].ip = ip;
            net_memcpy(g_cache[i].mac, mac, ETH_ALEN);
            g_cache[i].valid = 1;
            return;
        }
    }
    /* Evict entry 0 (simple LRU approximation) */
    g_cache[0].ip = ip;
    net_memcpy(g_cache[0].mac, mac, ETH_ALEN);
    g_cache[0].valid = 1;
}

int arp_lookup(ipv4_t ip, uint8_t *mac_out)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].valid && g_cache[i].ip == ip) {
            net_memcpy(mac_out, g_cache[i].mac, ETH_ALEN);
            return 1;
        }
    }
    return 0;
}

static void arp_send(uint16_t oper, const uint8_t *tha, ipv4_t tpa)
{
    uint8_t frame[ETH_HDR_LEN + sizeof(ArpPkt)];
    /* Ethernet header */
    const uint8_t *dst_mac = (oper == 1) ? ETH_BCAST : tha;
    eth_build(frame, dst_mac, g_my_mac, ETHERTYPE_ARP, (uint16_t)sizeof(ArpPkt));
    /* ARP payload */
    ArpPkt *a = (ArpPkt *)(frame + ETH_HDR_LEN);
    a->htype = net_htons(1);
    a->ptype = net_htons(0x0800);
    a->hlen  = ETH_ALEN;
    a->plen  = 4;
    a->oper  = net_htons(oper);
    net_memcpy(a->sha, g_my_mac, ETH_ALEN);
    a->spa   = net_htonl(g_my_ip);
    net_memcpy(a->tha, tha, ETH_ALEN);
    a->tpa   = net_htonl(tpa);
    netdev_send(frame, (uint16_t)sizeof(frame));
}

void arp_request(ipv4_t target_ip)
{
    static const uint8_t zero_mac[ETH_ALEN] = {0};
    arp_send(1, zero_mac, target_ip);
}

int arp_cache_dump(ArpDumpCb cb, void *ud)
{
    int count = 0;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].valid) {
            cb(g_cache[i].ip, g_cache[i].mac, ud);
            count++;
        }
    }
    return count;
}

void arp_rx(const uint8_t *pkt, uint16_t len)
{
    if (len < (uint16_t)sizeof(ArpPkt)) return;
    const ArpPkt *a = (const ArpPkt *)pkt;
    if (net_ntohs(a->htype) != 1)      return;
    if (net_ntohs(a->ptype) != 0x0800) return;
    if (a->hlen != ETH_ALEN || a->plen != 4) return;

    ipv4_t sender_ip = net_ntohl(a->spa);
    /* Always update cache with sender info */
    arp_cache_update(sender_ip, a->sha);

    if (net_ntohs(a->oper) == 1) {
        /* ARP Request: reply if we are the target */
        ipv4_t target_ip = net_ntohl(a->tpa);
        if (target_ip == g_my_ip) {
            arp_send(2, a->sha, sender_ip);
        }
    }
    /* ARP Reply: cache already updated above */
}
