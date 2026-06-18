/*
 * cmd_netstop.c — UAOS NetStop command
 *
 * Shuts down the network stack and releases the network interface.
 *
 * Usage: NetStop
 *
 * If the IP address was obtained via DHCP, this command sends a
 * DHCPRELEASE message to the server before shutting down.
 */
#include "cmd_internal.h"
#include "../net/stack.h"
#include "../net/net_device.h"

void Cmd_Netstop(NativeCmdCtx *ctx, const char *args)
{
    (void)ctx;
    (void)args;

    if (!net_stack_is_up()) {
        PRINT("NetStop: network is already down.");
        return;
    }

    /* Check if DHCP was used for informational message */
    int was_dhcp = net_stack_dhcp_used();

    /* Shutdown the network stack (sends DHCPRELEASE if needed) */
    PRINT("NetStop: shutting down network stack...");
    net_stack_shutdown();

    /* Shutdown the network device */
    netdev_shutdown();

    char line[80];
    cmd_scopy(line, "NetStop: network stopped", sizeof(line));
    if (was_dhcp) {
        cmd_scat(line, " (DHCP lease released)", sizeof(line));
    }
    PRINT(line);
}
