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
#include "../klog/klog.h"
#include "../exec/task.h"
#include "../drivers/entropy.h"

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
 * Returns  1 and fills *out_ip on success;
 *          0 if the packet isn't a usable reply to our query (keep waiting);
 *         -1 if the server gave a definitive answer we must NOT retry —
 *            an error RCODE (NXDOMAIN/SERVFAIL/REFUSED/…) or a valid
 *            response that simply contains no A record.
 * txid: the transaction ID we sent; response must match.
 * query/qlen: the question section we sent — a response whose question
 * doesn't echo it byte-for-byte is not a reply to our query (UAOS-168).
 * ------------------------------------------------------------------------- */
static int dns_parse_response(const uint8_t *buf, uint16_t len,
                              uint16_t txid, ipv4_t *out_ip,
                              const uint8_t *query, uint16_t qlen)
{
    if (len < DNS_HDR_LEN) return 0;

    const DnsHdr *hdr = (const DnsHdr *)buf;

    uint16_t rid   = net_ntohs(hdr->id);
    uint16_t flags = net_ntohs(hdr->flags);
    uint16_t ancount = net_ntohs(hdr->ancount);
    uint16_t qdcount = net_ntohs(hdr->qdcount);

    klog_puts(KLOG_DNS, KLOG_DEBUG, "rx id="); klog_appendf(KLOG_DNS, KLOG_DEBUG, "%04X", rid);
    klog_puts(KLOG_DNS, KLOG_DEBUG, " flags="); klog_appendf(KLOG_DNS, KLOG_DEBUG, "%04X", flags);
    klog_puts(KLOG_DNS, KLOG_DEBUG, " an="); klog_appendf(KLOG_DNS, KLOG_DEBUG, "%02X", (uint8_t)ancount);
    klog_putc(KLOG_DNS, KLOG_DEBUG, '\n');

    if (rid != txid) return 0;                     /* not our reply */
    if (!(flags & DNS_FLAG_QR)) return 0;          /* not a response */
    /* The answer must echo exactly the one question we sent — a packet
     * with our txid but a different question is a forgery (UAOS-168). */
    if (qdcount != 1 || len < qlen ||
        net_memcmp(buf + DNS_HDR_LEN, query + DNS_HDR_LEN,
                   (uint32_t)(qlen - DNS_HDR_LEN)) != 0)
        return 0;
    if ((flags & DNS_FLAG_RCODE) != 0) {           /* definitive error (NXDOMAIN, …) */
        klog_puts(KLOG_DNS, KLOG_DEBUG, "rcode="); klog_appendf(KLOG_DNS, KLOG_DEBUG, "%u", (unsigned)(flags & DNS_FLAG_RCODE));
        klog_putc(KLOG_DNS, KLOG_DEBUG, '\n');
        return -1;
    }
    if (ancount == 0) return -1;                   /* NODATA: definitive empty answer */

    /* Skip the question section */
    int off = DNS_HDR_LEN;
    for (uint16_t q = 0; q < qdcount; q++) {
        off = dns_name_end(buf, len, off);
        if (off < 0 || off + 4 > len) return -1;
        off += 4;   /* QTYPE + QCLASS */
    }

    /* Walk answer RRs looking for an A record */
    for (uint16_t a = 0; a < ancount; a++) {
        /* NAME field (may be a pointer) */
        off = dns_name_end(buf, len, off);
        if (off < 0 || off + 10 > len) return -1;

        uint16_t rtype  = (uint16_t)((buf[off] << 8) | buf[off+1]);
        /* uint16_t rclass = (uint16_t)((buf[off+2] << 8) | buf[off+3]); */
        /* uint32_t  ttl   = ... off+4 .. off+7 */
        uint16_t rdlen  = (uint16_t)((buf[off+8] << 8) | buf[off+9]);
        off += 10;

        klog_puts(KLOG_DNS, KLOG_DEBUG, "RR type="); klog_appendf(KLOG_DNS, KLOG_DEBUG, "%04X", rtype);
        klog_puts(KLOG_DNS, KLOG_DEBUG, " rdlen="); klog_appendf(KLOG_DNS, KLOG_DEBUG, "%02X", (uint8_t)rdlen); klog_putc(KLOG_DNS, KLOG_DEBUG, '\n');

        if (rtype == DNS_TYPE_A && rdlen == 4 && off + 4 <= len) {
            /* Found an A record */
            ipv4_t ip = ((uint32_t)buf[off]   << 24) |
                        ((uint32_t)buf[off+1] << 16) |
                        ((uint32_t)buf[off+2] <<  8) |
                         (uint32_t)buf[off+3];
            *out_ip = ip;
            klog_puts(KLOG_DNS, KLOG_DEBUG, "A record ip="); klog_appendf(KLOG_DNS, KLOG_DEBUG, "%08X", ip); klog_putc(KLOG_DNS, KLOG_DEBUG, '\n');
            return 1;
        }
        /* Skip this RR's RDATA */
        off += rdlen;
    }
    /* A real response arrived but contained no usable A record — the
     * answer is definitive, retrying won't change it. */
    return -1;
}

/* "localhost" (optionally with a trailing dot) resolves locally — no
 * network round trip, and it works with no DNS server configured. */
static int dns_is_localhost(const char *hostname)
{
    static const char l[] = "localhost";
    int i = 0;
    while (l[i]) {
        char c = hostname[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != l[i]) return 0;
        i++;
    }
    return hostname[i] == '\0' ||
           (hostname[i] == '.' && hostname[i + 1] == '\0');
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

    /* Fast path: localhost never hits the wire */
    if (dns_is_localhost(hostname)) {
        *out_ip = 0x7F000001;   /* 127.0.0.1 */
        return 1;
    }

    ipv4_t dns_server = net_stack_get_dns();
    if (!dns_server) {
        klog_puts(KLOG_DNS, KLOG_DEBUG, "no DNS server configured\n");
        return 0;
    }

    klog_puts(KLOG_DNS, KLOG_DEBUG, "resolve: "); klog_puts(KLOG_DNS, KLOG_DEBUG, hostname); klog_putc(KLOG_DNS, KLOG_DEBUG, '\n');
    klog_puts(KLOG_DNS, KLOG_DEBUG, "server="); klog_appendf(KLOG_DNS, KLOG_DEBUG, "%08X", dns_server); klog_putc(KLOG_DNS, KLOG_DEBUG, '\n');

    /* Random transaction ID from the kernel entropy source (UAOS-168).
     * The old hostname-derived ID was deterministic — an off-path
     * attacker who knew the name only had to guess the (then
     * sequential) source port to inject a forged A record. */
    uint16_t txid = 0;
    entropy_fill(&txid, sizeof(txid));
    txid |= 1;   /* ensure non-zero */

    /* Build query */
    uint8_t qbuf[280];
    uint16_t qlen = dns_build_query(qbuf, (uint16_t)sizeof(qbuf), txid, hostname);
    if (!qlen) {
        klog_puts(KLOG_DNS, KLOG_DEBUG, "query build failed (hostname too long?)\n");
        return 0;
    }

    /* Open ephemeral UDP socket */
    int sock = udp_open(0);
    if (sock < 0) {
        klog_puts(KLOG_DNS, KLOG_DEBUG, "no UDP socket available\n");
        return 0;
    }

    /* Retry loop: send query, wait up to 2 s per attempt, up to timeout_ms
     * total.  Retries happen only on real timeouts — a definitive answer
     * (any response with our txid and QR set, including NXDOMAIN) ends the
     * loop immediately. */
    static const uint32_t RETRY_MS  = 2000;
    static const uint32_t SLICE_MS  = 50;
    uint32_t elapsed = 0;
    int result = 0;
    int answered = 0;

    /* The poll_fn-less path blocks on SIGF_NET — arm once for the whole
     * resolve so NIC IRQs wake us instead of waiting for the slice
     * timeout (idempotent if the caller is already armed). */
    if (!poll_fn)
        net_rx_notify_arm();

    while (elapsed < timeout_ms && !result && !answered) {
        klog_puts(KLOG_DNS, KLOG_DEBUG, "sending query txid="); klog_appendf(KLOG_DNS, KLOG_DEBUG, "%04X", txid); klog_putc(KLOG_DNS, KLOG_DEBUG, '\n');
        udp_send(sock, dns_server, DNS_PORT, qbuf, qlen);

        /* Wait up to RETRY_MS for a response, polling in SLICE_MS slices */
        uint32_t waited = 0;
        while (waited < RETRY_MS && elapsed < timeout_ms && !result) {
            if (poll_fn) {
                poll_fn(poll_arg, SLICE_MS);
            } else {
                /* No caller poll hook: block on SIGF_NET until the NIC
                 * IRQ wakes us (armed below), then drain the stack.
                 * Replaces the old 5M-iteration pause spin per slice
                 * (UAOS-168).  The slice length is the safety-net
                 * timeout in case a reply arrives via a path that
                 * doesn't kick SIGF_NET. */
                Task_WaitTicks(SIGF_NET, (SLICE_MS + 9) / 10);
                net_stack_poll();
            }
            waited  += SLICE_MS;
            elapsed += SLICE_MS;

            /* Check for incoming UDP packet on our socket — only accept
             * answers from the server we actually queried (UAOS-168). */
            uint8_t rbuf[512];
            ipv4_t  src_ip   = 0;
            uint16_t src_port = 0;
            int rlen = udp_recv(sock, rbuf, (uint16_t)sizeof(rbuf),
                                &src_ip, &src_port);
            if (rlen > 0 && src_port == DNS_PORT && src_ip == dns_server) {
                int r = dns_parse_response(rbuf, (uint16_t)rlen, txid,
                                           out_ip, qbuf, qlen);
                if (r > 0) result = 1;
                else if (r < 0) { answered = 1; break; }
            }
        }
    }

    udp_close(sock);
    if (!poll_fn)
        net_rx_notify_disarm(Task_Current());

    if (!result) {
        klog_puts(KLOG_DNS, KLOG_DEBUG, answered ? "resolve failed (definitive answer)\n"
                                               : "resolve timed out\n");
    }
    return result;
}
