/*
 * net.h — UAOS TCP/IP stack: shared types and byte-order helpers
 */
#ifndef UAOS_NET_H
#define UAOS_NET_H

#include <stdint.h>
#include <stddef.h>

/* Byte-order conversion (x86 is little-endian, network is big-endian) */
static inline uint16_t net_htons(uint16_t v){ return (uint16_t)((v>>8)|(v<<8)); }
static inline uint16_t net_ntohs(uint16_t v){ return net_htons(v); }
static inline uint32_t net_htonl(uint32_t v){
    return ((v>>24)&0xFF)|((v>>8)&0xFF00)|((v<<8)&0xFF0000)|((v<<24)&0xFF000000UL);
}
static inline uint32_t net_ntohl(uint32_t v){ return net_htonl(v); }

/* Memcpy / memset (freestanding) */
static inline void net_memcpy(void *d, const void *s, uint32_t n){
    uint8_t *dp=(uint8_t*)d; const uint8_t *sp=(const uint8_t*)s;
    while(n--) *dp++=*sp++;
}
static inline void net_memset(void *d, uint8_t v, uint32_t n){
    uint8_t *dp=(uint8_t*)d; while(n--) *dp++=v;
}
static inline int net_memcmp(const void *a, const void *b, uint32_t n){
    const uint8_t *ap=(const uint8_t*)a, *bp=(const uint8_t*)b;
    while(n--){ if(*ap!=*bp) return (int)*ap-(int)*bp; ap++; bp++; }
    return 0;
}

/* IPv4 address as 32-bit big-endian (as stored in packets) */
typedef uint32_t ipv4_t;

/* MAC address */
#define ETH_ALEN 6
typedef struct { uint8_t b[ETH_ALEN]; } MacAddr;

/* IPv4 helper: build address from dotted quads */
#define IPV4(a,b,c,d) ((ipv4_t)(((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(d)))

/* Internet checksum over len bytes of data */
static inline uint16_t inet_cksum(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    /* Sum as big-endian 16-bit words (network byte order) */
    while (len > 1) {
        sum += (uint32_t)(((uint16_t)p[0] << 8) | p[1]);
        p += 2; len -= 2;
    }
    if (len) sum += (uint32_t)(*p) << 8;  /* odd byte in high position */
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

#endif /* UAOS_NET_H */
