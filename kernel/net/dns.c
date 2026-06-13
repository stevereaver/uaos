/*
 * dns.c — Minimal DNS A-record resolver (RFC 1035)
 *
 * Wire format recap (RFC 1035 §4):
 *
 *   Header  (12 bytes): id, flags, qdcount, ancount, nscount, arcount
 *   Question section  : QNAME (label sequence), QTYPE (2), QCLASS (2)
 *   Answer section    : NAME, TYPE, CLASS, TTL, RDLENGTH, RDATA
 *
 * QNAME encoding: each label is preceded by its length byte; the sequence
 * is terminated by a zero-length label (0x00).  Pointers (0xC0 xx) in
 * responses compress repeated domain names and must be followed.
 *
 * We send one question per call (QTYPE=A, QCLASS=IN) and parse only the
 * first A record in the answer section.
 */
#include "dns.h"
#include "udp.h"
#include "stack.h"
#include "net.h"

/* -------------------------------------------------------------------------
 * Serial debug (COM1) — same pattern as rest of net stack
 * ------------------------------------------------------------------------- */
static inline void _dn_outb(uint16_t p, uint8_t v)
{ __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t _dn_inb(uint16_t p)
{ uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static void _dn_putc(char c) {
    while ((_dn_inb(0x3FD) & 0x20) == 0) {}
    _dn_outb(0x3F8, (uint8_t)c);
    if (c == '\n') { while ((_dn_inb(0x3FD) & 0x20) == 0) {} _dn_outb(0x3F8, '\r'); }
}
static void _dn_puts(const char *s) { while (*s) _dn_putc(*s++); }
static void _dn_phex8(uint8_t v) {
    static const char h[] = "0123456789ABCDEF";
    _dn_putc(h[v >> 4]); _dn_putc(h[v & 0xF]);
}
static void _dn_phex16(uint16_t v) { _dn_phex8((uint8_t)(v >> 8)); _dn_phex8((uint8_t)v); }
static void _dn_phex32(uint32_t v) { _dn_phex16((uint16_t)(v >> 16)); _dn_phex16((uint16_t)v); }

/* -------------------------------------------------------------------------
 * Build DNS query packet.
 * Returns total packet length, or 0 if hostname is too long.
 *
 * Layout: DnsHdr | QNAME | QTYPE(2) | QCLASS(2)
 * ------------------------------------------------------------------------- */
static uint16_t dns_build_query(uint8_t *buf, uint16_t buflen,
                                uint16_t txid, const char *hostname)
{
    if (buflen < DNS_HDR_LEN + 4) return 0;

    /* Header */
    DnsHdr *hdr = (DnsHdr *)buf;
    hdr->id      = net_htons(txid);
    hdr->flags   = net_htons(DNS_FLAG_RD);   /* standard recursive query */
    hdr->qdcount = net_htons(1);
    hdr->ancount = 0;
    hdr->nscount = 0;
    hdr->arcount = 0;

    /* QNAME: encode "www.example.com" → \x03www\x07example\x03com\x00 */
    uint8_t *p = buf + DNS_HDR_LEN;
    uint8_t *end = buf + buflen - 4;  /* leave room for QTYPE+QCLASS */

    const char *src = hostname;
    while (*src) {
        /* Find end of this label */
        const char *dot = src;
        while (*dot && *dot != '.') dot++;
        uint8_t llen = (uint8_t)(dot - src);
        if (llen == 0 || llen > 63) return 0;   /* empty label or too long */
        if (p + 1 + llen >= end) return 0;       /* won't fit */
        *p++ = llen;
        while (src < dot) *p++ = (uint8_t)*src++;
        if (*src == '.') src++;   /* skip dot */
    }
    if (p >= end) return 0;
    *p++ = 0x00;   /* root label */

    /* QTYPE = A (1), QCLASS = IN (1) */
    *p++ = 0x00; *p++ = DNS_TYPE_A;
    *p++ = 0x00; *p++ = DNS_CLASS_IN;

    return (uint16_t)(p - buf);
}

/*
 * Advance past a DNS name field starting at offset off.
 * Returns the offset of the byte immediately after this name field
 * in the *original* buffer (a compression pointer counts as 2 bytes).
 */
static int dns_name_end(const uint8_t *buf, uint16_t buflen, int off)
{
    while (off < buflen) {
        uint8_t b = buf[off];
        if (b == 0) return off + 1;           /* end of name */
        if ((b & 0xC0) == 0xC0) return off + 2; /* pointer: 2 bytes, then done */
        off += 1 + (b & 0x3F);               /* skip label */
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Parse a DNS response and extract the first A record IP.
 * Returns 1 and fills *out_ip on success, 0 otherwise.
 * txid: the transaction ID we sent; response must match.
 * ------------------------------------------------------------------------- */
static int dns_parse_response(const uint8_t *buf, uint16_t len,
                              uint16_t txid, ipv4_t *out_ip)
{
    if (len < DNS_HDR_LEN) return 0;

    const DnsHdr *hdr = (const DnsHdr *)buf;

    uint16_t rid   = net_ntohs(hdr->id);
    uint16_t flags = net_ntohs(hdr->flags);
    uint16_t ancount = net_ntohs(hdr->ancount);
    uint16_t qdcount = net_ntohs(hdr->qdcount);

    _dn_puts("[DNS] rx id="); _dn_phex16(rid);
    _dn_puts(" flags="); _dn_phex16(flags);
    _dn_puts(" an="); _dn_phex8((uint8_t)ancount);
    _dn_putc('\n');

    if (rid != txid) return 0;                     /* not our reply */
    if (!(flags & DNS_FLAG_QR)) return 0;          /* not a response */
    if ((flags & DNS_FLAG_RCODE) != 0) return 0;   /* error response */
    if (ancount == 0) return 0;                    /* no answers */

    /* Skip the question section */
    int off = DNS_HDR_LEN;
    for (uint16_t q = 0; q < qdcount; q++) {
        off = dns_name_end(buf, len, off);
        if (off < 0 || off + 4 > len) return 0;
        off += 4;   /* QTYPE + QCLASS */
    }

    /* Walk answer RRs looking for an A record */
    for (uint16_t a = 0; a < ancount; a++) {
        /* NAME field (may be a pointer) */
        off = dns_name_end(buf, len, off);
        if (off < 0 || off + 10 > len) return 0;

        uint16_t rtype  = (uint16_t)((buf[off] << 8) | buf[off+1]);
        /* uint16_t rclass = (uint16_t)((buf[off+2] << 8) | buf[off+3]); */
        /* uint32_t  ttl   = ... off+4 .. off+7 */
        uint16_t rdlen  = (uint16_t)((buf[off+8] << 8) | buf[off+9]);
        off += 10;

        _dn_puts("[DNS] RR type="); _dn_phex16(rtype);
        _dn_puts(" rdlen="); _dn_phex8((uint8_t)rdlen); _dn_putc('\n');

        if (rtype == DNS_TYPE_A && rdlen == 4 && off + 4 <= len) {
            /* Found an A record */
            ipv4_t ip = ((uint32_t)buf[off]   << 24) |
                        ((uint32_t)buf[off+1] << 16) |
                        ((uint32_t)buf[off+2] <<  8) |
                         (uint32_t)buf[off+3];
            *out_ip = ip;
            _dn_puts("[DNS] A record ip="); _dn_phex32(ip); _dn_putc('\n');
            return 1;
        }
        /* Skip this RR's RDATA */
        off += rdlen;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
int dns_resolve(const char *hostname, ipv4_t *out_ip,
                uint32_t timeout_ms,
                DnsPollFn poll_fn, void *poll_arg)
{
    /* Fast path: already a dotted-decimal address */
    if (net_str_to_ip(hostname, out_ip)) return 1;

    ipv4_t dns_server = net_stack_get_dns();
    if (!dns_server) {
        _dn_puts("[DNS] no DNS server configured\n");
        return 0;
    }

    _dn_puts("[DNS] resolve: "); _dn_puts(hostname); _dn_putc('\n');
    _dn_puts("[DNS] server="); _dn_phex32(dns_server); _dn_putc('\n');

    /* Use a fixed transaction ID derived from the hostname for simplicity */
    uint16_t txid = 0xAB00;
    for (const char *p = hostname; *p; p++)
        txid = (uint16_t)(txid * 31 + (uint8_t)*p);
    txid |= 1;   /* ensure non-zero */

    /* Build query */
    uint8_t qbuf[280];
    uint16_t qlen = dns_build_query(qbuf, (uint16_t)sizeof(qbuf), txid, hostname);
    if (!qlen) {
        _dn_puts("[DNS] query build failed (hostname too long?)\n");
        return 0;
    }

    /* Open ephemeral UDP socket */
    int sock = udp_open(0);
    if (sock < 0) {
        _dn_puts("[DNS] no UDP socket available\n");
        return 0;
    }

    /* Retry loop: send query, wait up to 2 s per attempt, up to timeout_ms total */
    static const uint32_t RETRY_MS  = 2000;
    static const uint32_t SLICE_MS  = 50;
    uint32_t elapsed = 0;
    int result = 0;

    while (elapsed < timeout_ms && !result) {
        _dn_puts("[DNS] sending query txid="); _dn_phex16(txid); _dn_putc('\n');
        udp_send(sock, dns_server, DNS_PORT, qbuf, qlen);

        /* Wait up to RETRY_MS for a response, polling in SLICE_MS slices */
        uint32_t waited = 0;
        while (waited < RETRY_MS && elapsed < timeout_ms && !result) {
            if (poll_fn)
                poll_fn(poll_arg, SLICE_MS);
            else {
                /* Simple busy-poll without yielding */
                volatile uint32_t n = 5000000UL;
                while (n--) __asm__ volatile("pause");
                net_stack_poll();
            }
            waited  += SLICE_MS;
            elapsed += SLICE_MS;

            /* Check for incoming UDP packet on our socket */
            uint8_t rbuf[512];
            ipv4_t  src_ip   = 0;
            uint16_t src_port = 0;
            int rlen = udp_recv(sock, rbuf, (uint16_t)sizeof(rbuf),
                                &src_ip, &src_port);
            if (rlen > 0 && src_port == DNS_PORT) {
                if (dns_parse_response(rbuf, (uint16_t)rlen, txid, out_ip))
                    result = 1;
            }
        }
    }

    udp_close(sock);

    if (!result) _dn_puts("[DNS] resolve timed out\n");
    return result;
}
