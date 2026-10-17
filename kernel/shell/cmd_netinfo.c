/* cmd_netinfo.c — Tools:NetInfo — launch the NetInfo window
 *
 * On console/desktop shells this opens the framebuffer NetInfo window.
 * On remote (telnet) shells there is no framebuffer to view it on, so a
 * text summary of the same information is printed instead.
 */

#include "cmd_internal.h"
#include "../display/netinfo_win.h"
#include "../net/stack.h"
#include "../net/ip.h"
#include "../net/net_device.h"

void Cmd_NetInfo(NativeCmdCtx *ctx, const char *args)
{
    (void)args;

    if (ctx->remote) {
        if (!net_stack_is_up()) {
            PRINT("netinfo: network stack not available");
            return;
        }

        char line[96];
        char val[24];

        cmd_scopy(line, "Interface:  ", sizeof(line));
        cmd_scat(line, netdev_name(), sizeof(line));
        PRINT(line);

        cmd_scopy(line, "State:      ", sizeof(line));
        cmd_scat(line, netdev_is_up() ? "Up" : "Down", sizeof(line));
        PRINT(line);

        uint8_t mac[ETH_ALEN];
        netdev_get_mac(mac);
        cmd_scopy(line, "MAC:        ", sizeof(line));
        int l = cmd_slen(line);
        static const char hex[] = "0123456789ABCDEF";
        for (int b = 0; b < ETH_ALEN; b++) {
            line[l++] = hex[(mac[b] >> 4) & 0xF];
            line[l++] = hex[mac[b] & 0xF];
            if (b < ETH_ALEN - 1) line[l++] = ':';
        }
        line[l] = '\0';
        PRINT(line);

        net_ip_to_str(ip_get_local(), val);
        cmd_scopy(line, "IP:         ", sizeof(line));
        cmd_scat(line, val, sizeof(line));
        PRINT(line);

        net_ip_to_str(ip_get_netmask(), val);
        cmd_scopy(line, "Netmask:    ", sizeof(line));
        cmd_scat(line, val, sizeof(line));
        PRINT(line);

        net_ip_to_str(ip_get_gateway(), val);
        cmd_scopy(line, "Gateway:    ", sizeof(line));
        cmd_scat(line, val, sizeof(line));
        PRINT(line);

        ipv4_t dns = net_stack_get_dns();
        if (dns) {
            net_ip_to_str(dns, val);
            cmd_scopy(line, "DNS:        ", sizeof(line));
            cmd_scat(line, val, sizeof(line));
            PRINT(line);
        }
        return;
    }

    NetInfoWin_Open();
}
