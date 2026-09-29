/* uaos_socket.h — UAOS userspace socket API (x86-64)
 *
 * Header-only wrappers over the NET_* syscall block (0x38–0x3F) backed by
 * kernel/net/usock.c.  The kernel side implements blocking semantics:
 * connect/send/recv pump the net stack and sleep in 10 ms quanta until
 * completion, error, or the per-socket timeout, so callers see ordinary
 * synchronous socket behaviour.
 *
 * IPv4 addresses are passed as uint32_t in UAOS wire order — use
 * UAOS_IPV4(a,b,c,d) or take the value from uaos_resolve().
 */

#ifndef UAOS_SOCKET_WRAPPER_H
#define UAOS_SOCKET_WRAPPER_H

#include "uaos_syscall.h"

/* Socket types (kernel/net/usock.h) */
#define UAOS_SOCK_STREAM   1    /* TCP */

/* Socket options for uaos_sock_setopt (milliseconds; 0 = wait forever) */
#define UAOS_SOCKOPT_CONNECT_TIMEOUT   1
#define UAOS_SOCKOPT_RECV_TIMEOUT      2
#define UAOS_SOCKOPT_SEND_TIMEOUT      3

/* Error codes returned by the socket calls (also -errno-style) */
#define UAOS_EBADF      -1   /* bad/foreign/closed socket handle */
#define UAOS_ETIMEDOUT  -2   /* operation deadline expired */
#define UAOS_ECONN      -3   /* refused / reset / not connected / DNS fail */
#define UAOS_ENETDOWN   -4   /* network stack is not running (netstart) */
#define UAOS_EINVAL     -5   /* bad args / unsupported socket type */
#define UAOS_EMFILE     -6   /* no free socket slots */

/* TCP states (kernel/net/tcp.h TcpState) as returned by uaos_sock_state() */
#define UAOS_TCP_CLOSED       0
#define UAOS_TCP_SYN_SENT     1
#define UAOS_TCP_SYN_RECEIVED 2
#define UAOS_TCP_ESTABLISHED  3
#define UAOS_TCP_FIN_WAIT_1   4
#define UAOS_TCP_FIN_WAIT_2   5
#define UAOS_TCP_CLOSE_WAIT   6
#define UAOS_TCP_LAST_ACK     7
#define UAOS_TCP_TIME_WAIT    8
#define UAOS_TCP_LISTEN       9

/* IPv4 address in UAOS order: 10.0.2.15 -> 0x0A00020F */
#define UAOS_IPV4(a,b,c,d) \
    ((uint32_t)(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                ((uint32_t)(c) << 8) | (uint32_t)(d)))

static inline long uaos_socket(int type)
{
    return uaos_syscall1(UAOS_SYSCALL_NET_SOCKET, type);
}

/* Blocking connect.  Returns 0 on success, negative UAOS_E* on failure. */
static inline long uaos_connect(int sock, uint32_t ip, uint16_t port)
{
    return uaos_syscall3(UAOS_SYSCALL_NET_CONNECT, sock, (long)ip, (long)port);
}

/* Blocking send-all.  Returns bytes sent (== len on success), < 0 on error. */
static inline long uaos_sock_send(int sock, const void *buf, long len)
{
    return uaos_syscall3(UAOS_SYSCALL_NET_SEND, sock, (long)buf, len);
}

/* Blocking receive.  Returns bytes read, 0 on orderly EOF, < 0 on error. */
static inline long uaos_sock_recv(int sock, void *buf, long len)
{
    return uaos_syscall3(UAOS_SYSCALL_NET_RECV, sock, (long)buf, len);
}

static inline long uaos_sock_close(int sock)
{
    return uaos_syscall1(UAOS_SYSCALL_NET_CLOSE, sock);
}

/* Resolve a hostname via the configured DNS server.
 * Returns 0 and fills *ip_out (UAOS_IPV4 order) on success, < 0 on failure. */
static inline long uaos_resolve(const char *host, uint32_t *ip_out,
                                uint32_t timeout_ms)
{
    return uaos_syscall3(UAOS_SYSCALL_NET_RESOLVE, (long)host,
                         (long)ip_out, (long)timeout_ms);
}

static inline long uaos_sock_setopt(int sock, int opt, uint32_t val)
{
    return uaos_syscall3(UAOS_SYSCALL_NET_SETOPT, sock, (long)opt, (long)val);
}

/* Current TCP state (UAOS_TCP_*), or UAOS_EBADF for a bad handle. */
static inline long uaos_sock_state(int sock)
{
    return uaos_syscall1(UAOS_SYSCALL_NET_STATE, sock);
}

/* -------------------------------------------------------------------------
 * Dotted-quad helpers (freestanding; no libc)
 * ------------------------------------------------------------------------- */

/* Parse "a.b.c.d" into UAOS_IPV4 order.  Returns 1 on success, 0 on failure. */
static inline int uaos_str_to_ipv4(const char *s, uint32_t *out)
{
    uint32_t parts[4];
    for (int i = 0; i < 4; i++) {
        uint32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s - '0');
            if (v > 255) return 0;
            s++;
            digits++;
        }
        if (!digits) return 0;
        parts[i] = v;
        if (i < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    if (*s) return 0;          /* trailing junk */
    *out = UAOS_IPV4(parts[0], parts[1], parts[2], parts[3]);
    return 1;
}

/* Format a UAOS-order IPv4 as "a.b.c.d" into buf (>= 16 bytes). */
static inline void uaos_ipv4_to_str(uint32_t ip, char *buf)
{
    char tmp[16];
    int n = 0;
    for (int i = 3; i >= 0; i--) {
        uint32_t b = (ip >> (i * 8)) & 0xFF;
        if (b >= 100) tmp[n++] = (char)('0' + b / 100);
        if (b >= 10)  tmp[n++] = (char)('0' + (b / 10) % 10);
        tmp[n++] = (char)('0' + b % 10);
        if (i) tmp[n++] = '.';
    }
    tmp[n] = '\0';
    for (int i = 0; i <= n; i++) buf[i] = tmp[i];
}

/* Resolve host to IPv4, accepting either a DNS name or a literal
 * dotted quad.  Returns 0 on success, < 0 on failure. */
static inline long uaos_resolve_host(const char *host, uint32_t *ip_out,
                                     uint32_t timeout_ms)
{
    uint32_t lit;
    if (uaos_str_to_ipv4(host, &lit)) {
        *ip_out = lit;
        return 0;
    }
    return uaos_resolve(host, ip_out, timeout_ms);
}

#endif /* UAOS_SOCKET_WRAPPER_H */
