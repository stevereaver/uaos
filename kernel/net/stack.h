/*
 * stack.h — UAOS TCP/IP stack top-level init
 */
#ifndef UAOS_STACK_H
#define UAOS_STACK_H

#include "net.h"

/*
 * Initialise the full network stack.
 * Scans for VirtIO-Net, sets up driver.  Attempts DHCP first (up to
 * timeout_ms milliseconds).  If DHCP fails or timeout_ms is 0, falls back
 * to the provided static ip/gateway/netmask.
 *
 * ip, gateway, netmask are in host byte order (use IPV4() macro).
 * Pass timeout_ms=0 to skip DHCP and use static addresses directly.
 *
 * Returns 1 if VirtIO-Net was found and stack is up, 0 otherwise.
 */
int  net_stack_init(ipv4_t ip, ipv4_t gateway, ipv4_t netmask);

/* Same as net_stack_init but with explicit DHCP timeout (ms) */
int  net_stack_init_ex(ipv4_t fallback_ip, ipv4_t fallback_gw,
                       ipv4_t fallback_nm, uint32_t dhcp_timeout_ms);

/* Returns 1 if the current IP was obtained via DHCP */
int  net_stack_dhcp_used(void);

/* Returns 1 if the network stack is up */
int  net_stack_is_up(void);

/* Poll the stack (call from event loop if polling mode) */
void net_stack_poll(void);

/* Called from PIT tick handler for TCP timers */
void net_stack_tick(void);

/* Returns our assigned IP address */
ipv4_t net_stack_get_ip(void);

/* Fill buf (at least 18 chars) with "a.b.c.d" string */
void net_ip_to_str(ipv4_t ip, char *buf);

/* Parse "a.b.c.d" into an ipv4_t. Returns 0 on failure. */
int  net_str_to_ip(const char *s, ipv4_t *out);

#endif /* UAOS_STACK_H */
