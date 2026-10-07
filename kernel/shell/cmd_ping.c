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
    for (int seq = 1; seq <= count; seq++) {
        if (CMD_BREAK(ctx)) {
            PRINT("***Break");
            return;
        }
        icmp_clear_reply();
        icmp_ping(dst, (uint16_t)seq);
        sent++;

        /* Wait up to ~1000 ms for a reply, yielding each 100 ms slice */
        int got = 0;
        for (int t = 0; t < 10; t++) {
            CMD_YIELD(ctx, 100);
            if (CMD_BREAK(ctx)) {
                PRINT("***Break");
                return;
            }
            if (icmp_got_reply()) { got = 1; break; }
        }

        if (got) {
            char ip_s[20]; net_ip_to_str(dst, ip_s);
            cmd_scopy(line, "32 bytes from ", sizeof(line));
            cmd_scat(line, ip_s, sizeof(line));
            cmd_scat(line, ": icmp_seq=", sizeof(line));
            char num[8]; cmd_uint_to_dec((uint32_t)seq, num, 8);
            cmd_scat(line, num, sizeof(line));
            cmd_scat(line, " ttl=64", sizeof(line));
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
}
