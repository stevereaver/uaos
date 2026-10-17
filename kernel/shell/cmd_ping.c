/*
 * cmd_ping.c — UAOS ping command
 *
 * Usage: ping <host>           (sends 4 ICMP echo requests)
 *        ping <host> <count>
 *
 * Hostnames are resolved via the DNS resolver before pinging.
 */
#include "cmd_internal.h"
#include "../net/stack.h"
#include "../net/icmp.h"
#include "../net/ip.h"
#include "../net/net.h"
#include "../net/arp.h"
#include "../net/dns.h"

extern uint64_t Tickmon_TscHz(void);

static inline uint64_t ping_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Format microseconds as "<ms>.<frac>" (3 decimals) for RTT display. */
static void ping_fmt_us(uint32_t us, char *buf, int max)
{
    cmd_uint_to_dec(us / 1000, buf, max);
    cmd_scat(buf, ".", max);
    char frac[4];
    uint32_t f = us % 1000;
    frac[0] = (char)('0' + (f / 100) % 10);
    frac[1] = (char)('0' + (f / 10) % 10);
    frac[2] = (char)('0' + f % 10);
    frac[3] = '\0';
    cmd_scat(buf, frac, max);
}

/* DnsPollFn adapter: forward slice to CMD_YIELD */
static void ping_dns_poll(void *arg, uint32_t ms)
{
    CMD_YIELD((NativeCmdCtx *)arg, ms);
}

void Cmd_Ping(NativeCmdCtx *ctx, const char *args)
{
    if (!net_stack_is_up()) {
        PRINT("ping: network stack not available");
        return;
    }

    if (!args || !*args) {
        PRINT("Usage: ping <host> [count]");
        return;
    }

    /* Parse host */
    char host[256]; int i = 0;
    while (*args && *args != ' ' && i < 255) host[i++] = *args++;
    host[i] = '\0';
    while (*args == ' ') args++;

    /* Parse optional count */
    int count = 4;
    if (*args >= '1' && *args <= '9') {
        count = 0;
        while (*args >= '0' && *args <= '9') { count = count*10 + (*args++ - '0'); }
        if (count < 1) count = 1;
        if (count > 64) count = 64;
    }

    /* Resolve hostname (fast-path for dotted-decimal, DNS query otherwise) */
    ipv4_t dst = 0;
    if (!dns_resolve(host, &dst, 5000, ping_dns_poll, ctx)) {
        char line[80];
        cmd_scopy(line, "ping: cannot resolve '", sizeof(line));
        cmd_scat(line, host, sizeof(line));
        cmd_scat(line, "'", sizeof(line));
        PRINT(line);
        return;
    }

    char line[128];
    char dst_s[20];
    net_ip_to_str(dst, dst_s);
    cmd_scopy(line, "PING ", sizeof(line));
    cmd_scat(line, host, sizeof(line));
    /* Show resolved IP when host was a name, not already an address */
    if (dst_s[0] && cmd_seq(host, dst_s) == 0) {
        cmd_scat(line, " (", sizeof(line));
        cmd_scat(line, dst_s, sizeof(line));
        cmd_scat(line, ")", sizeof(line));
    }
    cmd_scat(line, ": 32 data bytes", sizeof(line));
    PRINT(line);

    /* Ensure we have an ARP entry for the gateway/target.
     * For remote hosts (e.g. 8.8.8.8) the nexthop is the gateway.
     * Retry up to ~1 second via CMD_YIELD so the UI stays responsive. */
    uint8_t gw_mac[ETH_ALEN];
    ipv4_t nexthop = dst;
    ipv4_t local = ip_get_local();
    ipv4_t nm    = ip_get_netmask();
    ipv4_t gw    = ip_get_gateway();
    if ((dst & nm) != (local & nm)) nexthop = gw;
    if (!arp_lookup(nexthop, gw_mac)) {
        for (int arp_try = 0; arp_try < 10 && !arp_lookup(nexthop, gw_mac); arp_try++) {
            arp_request(nexthop);
            CMD_YIELD(ctx, 100);
        }
        if (!arp_lookup(nexthop, gw_mac)) {
            PRINT("ping: no ARP reply from gateway - network unreachable");
            return;
        }
    }

    int sent = 0, received = 0;
    const uint64_t tsc_hz = Tickmon_TscHz();
    uint32_t rtt_min = 0, rtt_max = 0;
    uint64_t rtt_sum = 0;
    for (int seq = 1; seq <= count; seq++) {
        if (CMD_BREAK(ctx)) {
            PRINT("***Break");
            return;
        }
        icmp_clear_reply();
        uint64_t t0 = ping_rdtsc();
        icmp_ping(dst, (uint16_t)seq);
        sent++;

        /* Wait up to ~1000 ms for a reply; 5 ms slices keep the RTT
         * measurement meaningful without reworking reply matching. */
        int got = 0;
        for (int t = 0; t < 200; t++) {
            CMD_YIELD(ctx, 5);
            if (CMD_BREAK(ctx)) {
                PRINT("***Break");
                return;
            }
            if (icmp_got_reply()) { got = 1; break; }
        }

        if (got) {
            uint64_t dt = ping_rdtsc() - t0;
            char ip_s[20]; net_ip_to_str(dst, ip_s);
            cmd_scopy(line, "32 bytes from ", sizeof(line));
            cmd_scat(line, ip_s, sizeof(line));
            cmd_scat(line, ": icmp_seq=", sizeof(line));
            char num[8]; cmd_uint_to_dec((uint32_t)seq, num, 8);
            cmd_scat(line, num, sizeof(line));
            cmd_scat(line, " ttl=64", sizeof(line));
            if (tsc_hz) {
                uint32_t us = (uint32_t)(dt * 1000000 / tsc_hz);
                char ms_s[20];
                ping_fmt_us(us, ms_s, sizeof(ms_s));
                cmd_scat(line, " time=", sizeof(line));
                cmd_scat(line, ms_s, sizeof(line));
                cmd_scat(line, " ms", sizeof(line));
                if (!received || us < rtt_min) rtt_min = us;
                if (us > rtt_max) rtt_max = us;
                rtt_sum += us;
            }
            PRINT(line);
            received++;
        } else {
            cmd_scopy(line, "Request timeout for icmp_seq ", sizeof(line));
            char num[8]; cmd_uint_to_dec((uint32_t)seq, num, 8);
            cmd_scat(line, num, sizeof(line));
            PRINT(line);
        }
    }

    /* Summary */
    PRINT("");
    cmd_scopy(line, "--- ", sizeof(line));
    cmd_scat(line, host, sizeof(line));
    cmd_scat(line, " ping statistics ---", sizeof(line));
    PRINT(line);

    char ns[8], nr[8];
    cmd_uint_to_dec((uint32_t)sent,     ns, 8);
    cmd_uint_to_dec((uint32_t)received, nr, 8);
    cmd_scopy(line, ns, sizeof(line));
    cmd_scat(line, " packets transmitted, ", sizeof(line));
    cmd_scat(line, nr, sizeof(line));
    cmd_scat(line, " packets received", sizeof(line));
    PRINT(line);

    if (received && tsc_hz) {
        char mn[20], av[20], mx[20];
        ping_fmt_us(rtt_min, mn, sizeof(mn));
        ping_fmt_us((uint32_t)(rtt_sum / (uint64_t)received), av, sizeof(av));
        ping_fmt_us(rtt_max, mx, sizeof(mx));
        cmd_scopy(line, "rtt min/avg/max = ", sizeof(line));
        cmd_scat(line, mn, sizeof(line));
        cmd_scat(line, "/", sizeof(line));
        cmd_scat(line, av, sizeof(line));
        cmd_scat(line, "/", sizeof(line));
        cmd_scat(line, mx, sizeof(line));
        cmd_scat(line, " ms", sizeof(line));
        PRINT(line);
    }
}
