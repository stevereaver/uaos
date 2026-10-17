/*
 * icmp.c — ICMP ping implementation
 */
#include "icmp.h"
#include "ip.h"

static int     g_got_reply = 0;
static uint16_t g_expect_ident = 0x4141;
static uint16_t g_expect_seq   = 0;
static ipv4_t   g_expect_src   = 0;
static uint8_t  g_reply_ttl    = 0;
static ipv4_t   g_reply_src    = 0;

void icmp_rx(ipv4_t src_ip, uint8_t ttl, const uint8_t *pkt, uint16_t len)
{
    if (len < ICMP_HDR_LEN) return;
    const IcmpHdr *h = (const IcmpHdr *)pkt;

    if (h->type == ICMP_ECHO_REQUEST) {
        /* Build and send echo reply */
        uint8_t reply[ICMP_HDR_LEN + 64];
        uint16_t data_len = (uint16_t)(len - ICMP_HDR_LEN);
        if (data_len > 64) data_len = 64;

        IcmpHdr *r = (IcmpHdr *)reply;
        r->type     = ICMP_ECHO_REPLY;
        r->code     = 0;
        r->checksum = 0;
        r->ident    = h->ident;
        r->seq      = h->seq;
        net_memcpy(reply + ICMP_HDR_LEN, pkt + ICMP_HDR_LEN, data_len);
        r->checksum = net_htons(inet_cksum(reply, (uint32_t)(ICMP_HDR_LEN + data_len)));
        ip_send(src_ip, IP_PROTO_ICMP, reply, (uint16_t)(ICMP_HDR_LEN + data_len));

    } else if (h->type == ICMP_ECHO_REPLY) {
        /* Only count replies matching the outstanding request — a stray
         * echo reply must not satisfy a pending ping. */
        if (net_ntohs(h->ident) == g_expect_ident &&
            net_ntohs(h->seq)   == g_expect_seq &&
            (!g_expect_src || src_ip == g_expect_src)) {
            g_reply_ttl = ttl;
            g_reply_src = src_ip;
            g_got_reply = 1;
        }
    }
}

void icmp_ping(ipv4_t dst_ip, uint16_t seq)
{
    g_expect_src = dst_ip;
    g_expect_seq = seq;
    uint8_t pkt[ICMP_HDR_LEN + 32];
    IcmpHdr *h = (IcmpHdr *)pkt;
    h->type     = ICMP_ECHO_REQUEST;
    h->code     = 0;
    h->checksum = 0;
    h->ident    = net_htons(0x4141);
    h->seq      = net_htons(seq);
    /* Payload */
    for (int i = 0; i < 32; i++) pkt[ICMP_HDR_LEN + i] = (uint8_t)i;
    h->checksum = net_htons(inet_cksum(pkt, (uint32_t)(ICMP_HDR_LEN + 32)));
    ip_send(dst_ip, IP_PROTO_ICMP, pkt, (uint16_t)(ICMP_HDR_LEN + 32));
}

int  icmp_got_reply(void)  { return g_got_reply; }
void icmp_clear_reply(void){ g_got_reply = 0; }
uint8_t icmp_reply_ttl(void){ return g_reply_ttl; }
ipv4_t  icmp_reply_src(void){ return g_reply_src; }
