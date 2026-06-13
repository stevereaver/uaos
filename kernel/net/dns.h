/*
 * dns.h — Minimal DNS resolver (RFC 1035)
 *
 * Sends a single A-record query over UDP to the configured DNS server and
 * returns the first IPv4 address in the answer section.
 *
 * The resolver is synchronous-cooperative: it uses the netdev_poll() /
 * yield callback machinery so the desktop stays live while waiting.
 */
#ifndef UAOS_DNS_H
#define UAOS_DNS_H

#include "net.h"
#include <stdint.h>

/* DNS wire-format header (12 bytes) */
typedef struct __attribute__((packed)) {
    uint16_t id;        /* transaction ID */
    uint16_t flags;     /* QR|opcode|AA|TC|RD|RA|Z|RCODE */
    uint16_t qdcount;   /* questions */
    uint16_t ancount;   /* answers   */
    uint16_t nscount;   /* authority */
    uint16_t arcount;   /* additional */
} DnsHdr;

#define DNS_HDR_LEN  12
#define DNS_PORT     53

/* flags for a standard recursive query */
#define DNS_FLAG_RD      0x0100   /* recursion desired */
#define DNS_FLAG_QR      0x8000   /* response bit */
#define DNS_FLAG_RCODE   0x000F   /* response code mask */

/* DNS record types / classes */
#define DNS_TYPE_A       1
#define DNS_CLASS_IN     1

/*
 * Resolve a hostname to an IPv4 address using the stack's DNS server.
 *
 * Parameters:
 *   hostname    — NUL-terminated name to look up (e.g. "www.google.com")
 *   out_ip      — filled with the resolved address on success (host byte order)
 *   timeout_ms  — how long to wait in total (milliseconds)
 *   poll_fn     — called each ~50 ms slice to pump the network / UI;
 *                 pass NULL to busy-poll without yielding
 *   poll_arg    — opaque argument forwarded to poll_fn
 *
 * Returns 1 on success, 0 on failure (timeout, NXDOMAIN, no DNS server, …).
 *
 * Thread safety: not re-entrant — do not call from an interrupt handler.
 */
typedef void (*DnsPollFn)(void *arg, uint32_t ms);

int dns_resolve(const char *hostname, ipv4_t *out_ip,
                uint32_t timeout_ms,
                DnsPollFn poll_fn, void *poll_arg);

#endif /* UAOS_DNS_H */
