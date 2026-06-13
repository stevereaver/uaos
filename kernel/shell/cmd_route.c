/*
 * cmd_route.c — UAOS route command
 *
 * Usage: route              (show routing table and ARP cache)
 *
 * Displays:
 *   - Kernel routing table (local subnet + default gateway)
 *   - ARP cache (neighbour IP -> MAC mappings)
 */
#include "cmd_internal.h"
#include "../net/stack.h"
#include "../net/ip.h"
#include "../net/arp.h"
#include "../net/net.h"
#include "../net/net_device.h"

/* -------------------------------------------------------------------------
 * ARP cache callback context
 * ------------------------------------------------------------------------- */
typedef struct {
    NativeCmdCtx *ctx;
    int           count;
} ArpDumpCtx;

static void arp_print_entry(ipv4_t ip, const uint8_t *mac, void *ud)
{
    ArpDumpCtx *dc = (ArpDumpCtx *)ud;
    NativeCmdCtx *ctx = dc->ctx;

    char line[80];
    char ip_s[20];
    static const char hex[] = "0123456789ABCDEF";

    net_ip_to_str(ip, ip_s);

    /* Build:  "  <ip>  at  <mac>" */
    cmd_scopy(line, "  ", sizeof(line));
    cmd_scat(line, ip_s, sizeof(line));

    /* Pad IP to 17 chars for alignment */
    int pad = 17 - cmd_slen(ip_s);
    int pos = cmd_slen(line);
    while (pad-- > 0 && pos < (int)sizeof(line) - 1) line[pos++] = ' ';
    line[pos] = '\0';

    cmd_scat(line, "at  ", sizeof(line));
    int l = cmd_slen(line);
    for (int b = 0; b < ETH_ALEN; b++) {
        line[l++] = hex[(mac[b] >> 4) & 0xF];
        line[l++] = hex[mac[b] & 0xF];
        if (b < ETH_ALEN - 1) line[l++] = ':';
    }
    line[l] = '\0';
    PRINT(line);
    dc->count++;
}

/* -------------------------------------------------------------------------
 * Command entry point
 * ------------------------------------------------------------------------- */
void Cmd_Route(NativeCmdCtx *ctx, const char *args)
{
    (void)args;

    if (!net_stack_is_up()) {
        PRINT("route: network stack not available");
        return;
    }

    ipv4_t local = ip_get_local();
    ipv4_t nm    = ip_get_netmask();
    ipv4_t gw    = ip_get_gateway();
    const char *devname = netdev_name();

    /* ---- Routing table ---- */
    PRINT("Kernel routing table:");
    PRINT("  Destination      Netmask          Gateway          Flags  Iface");
    PRINT("  ---------------  ---------------  ---------------  -----  -----");

    char line[96];
    char dst_s[20], nm_s[20], gw_s[20];

    /* Helper: append str to line then space-pad the column to 'width' chars */
#define COL(str, width) do { \
        cmd_scat(line, (str), sizeof(line)); \
        int _n = (width) - cmd_slen(str); \
        int _p = cmd_slen(line); \
        while (_n-- > 0 && _p < (int)sizeof(line)-1) line[_p++] = ' '; \
        line[_p] = '\0'; \
    } while(0)

    /* Local subnet route */
    ipv4_t subnet = local & nm;
    net_ip_to_str(subnet, dst_s);
    net_ip_to_str(nm,     nm_s);

    cmd_scopy(line, "  ", sizeof(line));
    COL(dst_s,       17);
    cmd_scat(line, "  ", sizeof(line));
    COL(nm_s,        17);
    cmd_scat(line, "  ", sizeof(line));
    COL("link#local", 17);
    cmd_scat(line, "  U      ", sizeof(line));
    cmd_scat(line, devname, sizeof(line));
    PRINT(line);

    /* Default route */
    net_ip_to_str(gw, gw_s);

    cmd_scopy(line, "  ", sizeof(line));
    COL("0.0.0.0",   17);
    cmd_scat(line, "  ", sizeof(line));
    COL("0.0.0.0",   17);
    cmd_scat(line, "  ", sizeof(line));
    COL(gw_s,        17);
    cmd_scat(line, "  UG     ", sizeof(line));
    cmd_scat(line, devname, sizeof(line));
    PRINT(line);

#undef COL

    /* ---- ARP cache ---- */
    PRINT("");
    PRINT("ARP cache:");

    ArpDumpCtx dc;
    dc.ctx   = ctx;
    dc.count = 0;
    arp_cache_dump(arp_print_entry, &dc);

    if (dc.count == 0)
        PRINT("  (empty)");
}
