/*
 * dhcp.h — Minimal DHCP client (DISCOVER → OFFER → REQUEST → ACK)
 */
#ifndef UAOS_DHCP_H
#define UAOS_DHCP_H

#include "net.h"

/* DHCP result */
typedef struct {
    ipv4_t ip;
    ipv4_t netmask;
    ipv4_t gateway;
    ipv4_t dns;
    uint32_t lease_secs;
} DhcpLease;

/*
 * Run DHCP discovery.  Sends DISCOVER, waits up to timeout_ms for OFFER,
 * sends REQUEST, waits for ACK.  Returns 1 on success and fills *lease.
 * Returns 0 on timeout/failure.
 *
 * Must be called after netdev_init() and before ip_init().
 */
int dhcp_request(DhcpLease *lease, uint32_t timeout_ms);

/* Renew an existing lease (sends REQUEST with known server IP). */
int dhcp_renew(DhcpLease *lease, uint32_t timeout_ms);

#endif /* UAOS_DHCP_H */
