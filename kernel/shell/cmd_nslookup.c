/*
 * cmd_nslookup.c — UAOS nslookup command
 *
 * Usage: nslookup <hostname>          (resolve using configured DNS server)
 *        nslookup <hostname> <server> (resolve using a specific DNS server)
 */
#include "cmd_internal.h"
#include "../net/stack.h"
#include "../net/dns.h"
#include "../net/net.h"

/* DnsPollFn adapter: forward each slice to CMD_YIELD */
static void nslookup_poll(void *arg, uint32_t ms)
{
    CMD_YIELD((NativeCmdCtx *)arg, ms);
}

void Cmd_Nslookup(NativeCmdCtx *ctx, const char *args)
{
    if (!net_stack_is_up()) {
        PRINT("nslookup: network stack not available");
        return;
    }

    if (!args || !*args) {
        PRINT("Usage: nslookup <hostname> [server]");
        return;
    }

    /* Parse hostname */
    char host[256]; int i = 0;
    while (*args && *args != ' ' && i < 255) host[i++] = *args++;
    host[i] = '\0';
    while (*args == ' ') args++;

    /* Optional: explicit DNS server override */
    ipv4_t saved_dns = 0;
    if (*args) {
        char srv_s[20]; int j = 0;
        while (*args && *args != ' ' && j < 19) srv_s[j++] = *args++;
        srv_s[j] = '\0';
        ipv4_t srv = 0;
        if (!net_str_to_ip(srv_s, &srv)) {
            PRINT("nslookup: invalid server address");
            return;
        }
        saved_dns = net_stack_get_dns();
        net_stack_set_dns(srv);
    }

    /* Print server being used */
    ipv4_t dns_ip = net_stack_get_dns();
    char line[128];
    char dns_s[20];

    if (!dns_ip) {
        PRINT("nslookup: no DNS server configured (run DHCP first)");
        if (saved_dns) net_stack_set_dns(saved_dns);
        return;
    }

    net_ip_to_str(dns_ip, dns_s);
    cmd_scopy(line, "Server:  ", sizeof(line));
    cmd_scat(line, dns_s, sizeof(line));
    PRINT(line);
    PRINT("");

    /* Resolve */
    ipv4_t result = 0;
    int ok = dns_resolve(host, &result, 5000, nslookup_poll, ctx);

    if (!ok) {
        cmd_scopy(line, "*** ", sizeof(line));
        cmd_scat(line, dns_s, sizeof(line));
        cmd_scat(line, " can't find ", sizeof(line));
        cmd_scat(line, host, sizeof(line));
        cmd_scat(line, ": NXDOMAIN", sizeof(line));
        PRINT(line);
    } else {
        char ip_s[20];
        net_ip_to_str(result, ip_s);

        cmd_scopy(line, "Name:    ", sizeof(line));
        cmd_scat(line, host, sizeof(line));
        PRINT(line);

        cmd_scopy(line, "Address: ", sizeof(line));
        cmd_scat(line, ip_s, sizeof(line));
        PRINT(line);
    }

    /* Restore overridden DNS server if we changed it */
    if (saved_dns) net_stack_set_dns(saved_dns);
}
