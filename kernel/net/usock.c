/*
 * usock.c — userspace socket layer for native x86-64 tasks
 *
 * Small blocking socket API layered over kernel/net tcp.c + dns.c.
 * The kernel TCP primitives are non-blocking (tcp_send returns 0 while a
 * segment is in flight, tcp_recv returns 0 while the RX ring is empty),
 * so each operation here pumps net_stack_poll() and sleeps in
 * Task_SleepTicks() quanta until completion, EOF, or a per-socket
 * deadline.  Sockets are owned by the creating task and reclaimed by
 * usock_cleanup_task() from Task_Exit so a killed tool cannot leak slots.
 */
#include "usock.h"
#include "tcp.h"
#include "dns.h"
#include "stack.h"
#include "../exec/task.h"
#include <stddef.h>

extern volatile uint64_t g_pit_ticks;   /* 100 Hz — uaos_kernel_main.c */

#define MAX_USOCKS      8
#define MS_TO_TICKS(ms) (((ms) + 9) / 10)

/* Default operation deadlines (ms); 0 would mean "wait forever". */
#define USOCK_CONN_TO_DFLT   8000
#define USOCK_RECV_TO_DFLT   30000
#define USOCK_SEND_TO_DFLT   30000

typedef struct {
    int      used;
    UaosTask *owner;        /* creating task; ops from other tasks fail */
    int      nsock;         /* tcp.c socket index, -1 until connected */
    int      type;
    uint32_t conn_to_ms;
    uint32_t recv_to_ms;
    uint32_t send_to_ms;
} Usock;

static Usock g_usocks[MAX_USOCKS];

static Usock *usock_lookup(int us)
{
    if (us < 0 || us >= MAX_USOCKS || !g_usocks[us].used)
        return NULL;
    if (g_usocks[us].owner != Task_Current())
        return NULL;
    return &g_usocks[us];
}

/* Tear down the native socket: graceful FIN while the stack is up,
 * abort when it is down (a FIN could never be answered or retransmitted). */
static void usock_release_native(Usock *u)
{
    if (u->nsock >= 0) {
        if (net_stack_is_up())
            tcp_close(u->nsock);
        else
            tcp_abort(u->nsock);
        u->nsock = -1;
    }
}

/* -------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

int usock_socket(int type)
{
    if (type != USOCK_STREAM)
        return USOCK_EINVAL;
    if (!net_stack_is_up())
        return USOCK_ENETDOWN;

    for (int i = 0; i < MAX_USOCKS; i++) {
        if (!g_usocks[i].used) {
            g_usocks[i].used       = 1;
            g_usocks[i].owner      = Task_Current();
            g_usocks[i].nsock      = -1;
            g_usocks[i].type       = type;
            g_usocks[i].conn_to_ms = USOCK_CONN_TO_DFLT;
            g_usocks[i].recv_to_ms = USOCK_RECV_TO_DFLT;
            g_usocks[i].send_to_ms = USOCK_SEND_TO_DFLT;
            return i;
        }
    }
    return USOCK_EMFILE;
}

int usock_connect(int us, uint32_t ip, uint16_t port)
{
    Usock *u = usock_lookup(us);
    if (!u)
        return USOCK_EBADF;
    if (u->nsock >= 0)
        return USOCK_EINVAL;            /* already connected/connecting */
    if (!net_stack_is_up())
        return USOCK_ENETDOWN;

    int ns = tcp_connect((ipv4_t)ip, port, 0);
    if (ns < 0)
        return USOCK_EMFILE;

    /* tcp_connect() returns the socket in SYN_SENT; wait for the
     * handshake.  tcp_tick() already bounds half-opens at
     * TCP_CONN_TIMEOUT_TICKS (7.5 s) and drops the socket to CLOSED —
     * the same signal a RST produces — so CLOSED means "failed". */
    uint64_t deadline = g_pit_ticks + MS_TO_TICKS(u->conn_to_ms);
    for (;;) {
        TcpState st = tcp_state(ns);
        if (st == TCP_ESTABLISHED) {
            u->nsock = ns;
            return USOCK_OK;
        }
        if (st == TCP_CLOSED)
            return USOCK_ECONN;
        if (u->conn_to_ms && g_pit_ticks >= deadline) {
            tcp_abort(ns);
            return USOCK_ETIMEDOUT;
        }
        net_stack_poll();
        Task_SleepTicks(1);
    }
}

int usock_send(int us, const void *buf, int len)
{
    Usock *u = usock_lookup(us);
    if (!u || u->nsock < 0 || !buf || len < 0)
        return USOCK_EBADF;
    if (!net_stack_is_up())
        return USOCK_ENETDOWN;

    const uint8_t *p = (const uint8_t *)buf;
    int total = 0;
    uint64_t deadline = u->send_to_ms
                        ? g_pit_ticks + MS_TO_TICKS(u->send_to_ms) : 0;
    while (total < len) {
        int n = tcp_send(u->nsock, p + total, (uint16_t)(len - total));
        if (n > 0) {
            total += n;
            continue;
        }
        TcpState st = tcp_state(u->nsock);
        if (st != TCP_ESTABLISHED && st != TCP_CLOSE_WAIT)
            return total ? total : USOCK_ECONN;
        if (deadline && g_pit_ticks >= deadline)
            return total ? total : USOCK_ETIMEDOUT;
        net_stack_poll();
        Task_SleepTicks(1);
    }
    return total;
}

int usock_recv(int us, void *buf, int len)
{
    Usock *u = usock_lookup(us);
    if (!u || u->nsock < 0 || !buf || len <= 0)
        return USOCK_EBADF;

    uint64_t deadline = u->recv_to_ms
                        ? g_pit_ticks + MS_TO_TICKS(u->recv_to_ms) : 0;
    for (;;) {
        int n = tcp_recv(u->nsock, (uint8_t *)buf, (uint16_t)len);
        if (n > 0)
            return n;

        TcpState st = tcp_state(u->nsock);
        if (st == TCP_CLOSED || st == TCP_CLOSE_WAIT || st == TCP_LAST_ACK ||
            st == TCP_TIME_WAIT)
            return 0;    /* peer closed and RX ring is drained: EOF */
        if (st != TCP_ESTABLISHED && st != TCP_FIN_WAIT_1 &&
            st != TCP_FIN_WAIT_2)
            return USOCK_ECONN;   /* SYN_SENT/LISTEN — not a live conn */
        if (deadline && g_pit_ticks >= deadline)
            return USOCK_ETIMEDOUT;
        net_stack_poll();
        Task_SleepTicks(1);
    }
}

int usock_close(int us)
{
    Usock *u = usock_lookup(us);
    if (!u)
        return USOCK_EBADF;
    usock_release_native(u);
    u->used  = 0;
    u->owner = NULL;
    return USOCK_OK;
}

int usock_state(int us)
{
    Usock *u = usock_lookup(us);
    if (!u)
        return USOCK_EBADF;
    if (u->nsock < 0)
        return (int)TCP_CLOSED;
    return (int)tcp_state(u->nsock);
}

int usock_setopt(int us, int opt, uint32_t val)
{
    Usock *u = usock_lookup(us);
    if (!u)
        return USOCK_EBADF;
    switch (opt) {
    case USOCK_OPT_CONNECT_TIMEOUT: u->conn_to_ms = val; return USOCK_OK;
    case USOCK_OPT_RECV_TIMEOUT:    u->recv_to_ms = val; return USOCK_OK;
    case USOCK_OPT_SEND_TIMEOUT:    u->send_to_ms = val; return USOCK_OK;
    }
    return USOCK_EINVAL;
}

/* DNS poll callback: pump the network, then park the task for the slice
 * so a resolving tool does not busy-spin (dns.c calls this every ~50 ms). */
static void usock_dns_poll(void *arg, uint32_t ms)
{
    (void)arg;
    net_stack_poll();
    uint64_t t = MS_TO_TICKS(ms);
    if (t)
        Task_SleepTicks(t);
}

int usock_resolve(const char *hostname, uint32_t *ip_out, uint32_t timeout_ms)
{
    if (!hostname || !ip_out)
        return USOCK_EINVAL;
    if (!net_stack_is_up())
        return USOCK_ENETDOWN;

    ipv4_t ip = 0;
    if (!dns_resolve(hostname, &ip, timeout_ms, usock_dns_poll, NULL))
        return USOCK_ECONN;             /* timeout / NXDOMAIN / no server */
    *ip_out = (uint32_t)ip;
    return USOCK_OK;
}

void usock_cleanup_task(void *task)
{
    for (int i = 0; i < MAX_USOCKS; i++) {
        if (g_usocks[i].used && g_usocks[i].owner == task) {
            usock_release_native(&g_usocks[i]);
            g_usocks[i].used  = 0;
            g_usocks[i].owner = NULL;
        }
    }
}
