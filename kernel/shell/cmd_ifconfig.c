/*
 * cmd_ifconfig.c — UAOS ifconfig command
 *
 * Usage: ifconfig              (show network configuration)
 *        ifconfig <ip> <gw>    (set IP and gateway, /24 netmask assumed)
 */
#include "cmd_internal.h"
#include "../net/stack.h"
#include "../net/ip.h"
#include "../net/net.h"
#include "../drivers/virtio_net.h"

void Cmd_Ifconfig(NativeCmdCtx *ctx, const char *args)
{
    /* Set mode: ifconfig <ip> <gw> */
    if (args && *args) {
        /* Parse IP */
        char ip_s[20]; int i = 0;
        while (*args && *args != ' ' && i < 19) ip_s[i++] = *args++;
        ip_s[i] = '\0';
        while (*args == ' ') args++;
        char gw_s[20]; i = 0;
        while (*args && *args != ' ' && i < 19) gw_s[i++] = *args++;
        gw_s[i] = '\0';

        ipv4_t ip = 0, gw = 0;
        if (!net_str_to_ip(ip_s, &ip) || !net_str_to_ip(gw_s, &gw)) {
            PRINT("Usage: ifconfig [<ip> <gateway>]");
            return;
        }
        /* Re-init IP layer with new addresses, /24 netmask */
        ipv4_t nm = IPV4(255,255,255,0);
        ip_init(ip, gw, nm);
        PRINT("Network reconfigured.");
        return;
    }

    /* Show mode */
    if (!net_stack_is_up()) {
        PRINT("vio0: VirtIO-Net  (no device found)");
        return;
    }

    char line[96];
    uint8_t mac[ETH_ALEN];
    virtio_net_get_mac(mac);

    /* MAC address */
    cmd_scopy(line, "vio0: flags=8843<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST>", sizeof(line));
    PRINT(line);

    /* inet */
    char ip_s[20], nm_s[20];
    net_ip_to_str(ip_get_local(),   ip_s);
    net_ip_to_str(ip_get_netmask(), nm_s);
    cmd_scopy(line, "      inet ", sizeof(line));
    cmd_scat(line, ip_s, sizeof(line));
    cmd_scat(line, "  netmask ", sizeof(line));
    cmd_scat(line, nm_s, sizeof(line));
    /* broadcast */
    char bc_s[20];
    ipv4_t bc = ip_get_local() | ~ip_get_netmask();
    net_ip_to_str(bc, bc_s);
    cmd_scat(line, "  broadcast ", sizeof(line));
    cmd_scat(line, bc_s, sizeof(line));
    PRINT(line);

    /* gateway */
    char gw_s[20];
    net_ip_to_str(ip_get_gateway(), gw_s);
    cmd_scopy(line, "      gateway ", sizeof(line));
    cmd_scat(line, gw_s, sizeof(line));
    PRINT(line);

    /* ether (MAC) */
    static const char hex[] = "0123456789ABCDEF";
    cmd_scopy(line, "      ether ", sizeof(line));
    int pos = cmd_slen(line);
    for (int b = 0; b < ETH_ALEN; b++) {
        line[pos++] = hex[(mac[b]>>4)&0xF];
        line[pos++] = hex[mac[b]&0xF];
        if (b < ETH_ALEN-1) line[pos++] = ':';
    }
    line[pos] = '\0';
    PRINT(line);

    /* media */
    PRINT("      media: Ethernet 1000baseT <full-duplex> (VirtIO)");
}
