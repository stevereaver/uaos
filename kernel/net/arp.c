/*
 * arp.c — ARP implementation
 */
#include "arp.h"
#include "eth.h"
#include "ip.h"
#include "net_device.h"

extern volatile uint64_t g_pit_ticks;      /* 100 Hz */

static ipv4_t   g_my_ip   = 0;
static ipv4_t   g_gateway = 0;
static uint8_t  g_my_mac[ETH_ALEN];

/* ARP cache — LRU with a pinned gateway entry (UAOS-166).  last_use is
 * stamped on insert, refresh, and lookup hit; eviction picks the oldest
 * non-pinned slot instead of always slot 0 (which used to be the first
 * entry learned — normally the default gateway). */
typedef struct {
    ipv4_t   ip;
    uint8_t  mac[ETH_ALEN];
    uint8_t  valid;
    uint8_t  pinned;        /* never evicted (the default gateway) */
    uint64_t last_use;
} ArpEntry;
static ArpEntry g_cache[ARP_CACHE_SIZE];

void arp_init(ipv4_t my_ip, const uint8_t *my_mac)
{
    g_my_ip   = my_ip;
    g_gateway = 0;
    net_memcpy(g_my_mac, my_mac, ETH_ALEN);
    net_memset(g_cache, 0, sizeof(g_cache));
}

void arp_set_gateway(ipv4_t gw_ip)
{
    g_gateway = gw_ip;
    /* Pin the entry too if the gateway is already cached. */
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (g_cache[i].valid && g_cache[i].ip == gw_ip)
            g_cache[i].pinned = 1;
}

static int arp_cache_find(ipv4_t ip)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (g_cache[i].valid && g_cache[i].ip == ip)
            return i;
    return -1;
}

void arp_cache_update(ipv4_t ip, const uint8_t *mac)
{
    if (!ip) return;
    /* Refresh existing entry */
    int i = arp_cache_find(ip);
    if (i >= 0) {
        net_memcpy(g_cache[i].mac, mac, ETH_ALEN);
        g_cache[i].last_use = g_pit_ticks;
        if (ip == g_gateway) g_cache[i].pinned = 1;
        return;
    }
    /* Empty slot first */
    int slot = -1;
    for (i = 0; i < ARP_CACHE_SIZE; i++)
        if (!g_cache[i].valid) { slot = i; break; }
    /* Full: evict the least-recently-used non-pinned entry */
    if (slot < 0) {
        uint64_t oldest = ~0ULL;
        for (i = 0; i < ARP_CACHE_SIZE; i++) {
            if (g_cache[i].pinned) continue;
            if (g_cache[i].last_use < oldest) {
                oldest = g_cache[i].last_use;
                slot = i;
            }
        }
        if (slot < 0) return;   /* every entry pinned — drop the new one */
    }
    g_cache[slot].ip       = ip;
    g_cache[slot].pinned   = (ip == g_gateway);
    g_cache[slot].last_use = g_pit_ticks;
    net_memcpy(g_cache[slot].mac, mac, ETH_ALEN);
    g_cache[slot].valid = 1;
}

int arp_lookup(ipv4_t ip, uint8_t *mac_out)
{
    int i = arp_cache_find(ip);
    if (i >= 0) {
        net_memcpy(mac_out, g_cache[i].mac, ETH_ALEN);
        g_cache[i].last_use = g_pit_ticks;   /* LRU touch */
        return 1;
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
    ipv4_t target_ip = net_ntohl(a->tpa);
    int is_reply = (net_ntohs(a->oper) == 2);

    /* RFC 826 merge: refresh the sender's MAC only if it already has an
     * entry; create a new entry only when WE are the target of the
     * packet.  Learning from every broadcast request lets chatty LAN
     * hosts fill the cache and evict the gateway (UAOS-166). */
    int merged = (arp_cache_find(sender_ip) >= 0);
    if (merged)
        arp_cache_update(sender_ip, a->sha);

    int learned = 0;
    if (target_ip == g_my_ip) {
        if (!merged) {
            arp_cache_update(sender_ip, a->sha);
            learned = 1;
        }
        if (!is_reply)      /* ARP request for us — answer it */
            arp_send(2, a->sha, sender_ip);
    }

    /* A learned/refreshed mapping may resolve a next-hop that has TX
     * frames queued on an ARP miss — flush them (UAOS-167). */
    if (merged || learned)
        ip_arp_resolved(sender_ip);
}
