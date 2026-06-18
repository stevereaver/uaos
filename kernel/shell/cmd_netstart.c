/*
 * cmd_netstart.c — UAOS NetStart command
 *
 * Initialises the TCP/IP stack and bsdsocket.library.
 * Reads its configuration from S:net.conf:
 *
 *   dhcp                     (try DHCP, fall back to static values)
 *   static                   (use the static values directly)
 *
 * When mode is dhcp, static values are optional fallback.
 * When mode is static, the next non-comment lines declare:
 *   address, netmask, gateway, dns
 *
 * Comments start with ';' or '#' and blank lines are ignored.
 */
#include "cmd_internal.h"
#include "../net/stack.h"
#include "../exec/bsdsocket_lib.h"

#define NET_CONF_PATH  "S:net.conf"

/* -------------------------------------------------------------------------
 * Parse S:net.conf
 * Returns number of values found (0 = file missing/unreadable,
 *                                 1 = mode only,
 *                                 5 = mode + 4 values).
 * ------------------------------------------------------------------------- */
static int parse_net_conf(char *mode, ipv4_t *ip, ipv4_t *nm, ipv4_t *gw, ipv4_t *dns)
{
    VfsFile fh;
    if (!VFS_Open(&fh, NET_CONF_PATH, VFS_READ)) return 0;

    uint32_t size = VFS_Size(&fh);
    if (size == 0 || size > 4095) { VFS_Close(&fh); return 0; }

    static char buf[4096];
    uint32_t nr = VFS_Read(&fh, (uint8_t *)buf, size);
    VFS_Close(&fh);
    buf[nr] = '\0';

    char tokens[5][64];
    int tok_count = 0;

    const char *p = buf;
    while (*p && tok_count < 5) {
        /* Skip leading whitespace */
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p) break;

        /* Skip comment lines */
        if (*p == '#' || *p == ';') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }

        /* Skip blank lines */
        if (*p == '\n') { p++; continue; }

        /* Copy token until end-of-line */
        int i = 0;
        while (*p && *p != '\r' && *p != '\n' && i < 63)
            tokens[tok_count][i++] = *p++;

        /* Trim trailing whitespace */
        while (i > 0 && (tokens[tok_count][i-1] == ' ' ||
                         tokens[tok_count][i-1] == '\t')) i--;
        tokens[tok_count][i] = '\0';

        if (i > 0) tok_count++;

        if (*p == '\r') p++;
        if (*p == '\n') p++;
    }

    if (tok_count == 0) return 0;

    int j = 0;
    while (tokens[0][j] && j < 63) {
        char c = tokens[0][j];
        if (c >= 'A' && c <= 'Z') c += 32;
        mode[j] = c;
        j++;
    }
    mode[j] = '\0';

    *ip = *nm = *gw = *dns = 0;
    if (tok_count > 1) net_str_to_ip(tokens[1], ip);
    if (tok_count > 2) net_str_to_ip(tokens[2], nm);
    if (tok_count > 3) net_str_to_ip(tokens[3], gw);
    if (tok_count > 4) net_str_to_ip(tokens[4], dns);

    return tok_count;
}

/* -------------------------------------------------------------------------
 * Command entry point
 * ------------------------------------------------------------------------- */
void Cmd_Netstart(NativeCmdCtx *ctx, const char *args)
{
    (void)args;

    char mode[16];
    ipv4_t ip = 0, nm = 0, gw = 0, dns = 0;
    int vals = parse_net_conf(mode, &ip, &nm, &gw, &dns);

    /* Hard-coded defaults when config is missing */
    if (vals == 0) {
        cmd_scopy(mode, "dhcp", sizeof(mode));
        ip  = IPV4(10,0,2,15);
        nm  = IPV4(255,255,255,0);
        gw  = IPV4(10,0,2,2);
        dns = IPV4(8,8,8,8);
    }

    int is_dhcp = (mode[0] == 'd');  /* "dhcp" */

    /* Validate static values */
    if (!is_dhcp && (ip == 0 || nm == 0 || gw == 0)) {
        PRINT("NetStart: static mode selected but address/netmask/gateway missing in S:net.conf");
        return;
    }

    {
        char line[80];
        if (is_dhcp) {
            cmd_scopy(line, "NetStart: DHCP mode (fallback ", sizeof(line));
            char ipbuf[20];
            net_ip_to_str(ip, ipbuf);
            cmd_scat(line, ipbuf, sizeof(line));
            cmd_scat(line, ")", sizeof(line));
        } else {
            cmd_scopy(line, "NetStart: static mode ", sizeof(line));
            char ipbuf[20];
            net_ip_to_str(ip, ipbuf);
            cmd_scat(line, ipbuf, sizeof(line));
        }
        PRINT(line);
    }

    int ok;
    if (is_dhcp) {
        /* Try DHCP; if we have fallback values, pass them.
         * Use 15s timeout for live CD environments where network
         * takes longer to stabilize after link-up. */
        if (ip) {
            ok = net_stack_init_ex(ip, gw, nm, 15000);
        } else {
            ok = net_stack_init_ex(0, 0, 0, 15000);
        }
    } else {
        ok = net_stack_init_ex(ip, gw, nm, 0);  /* timeout 0 = skip DHCP */
    }

    if (!ok) {
        PRINT("NetStart: no network device found.");
        return;
    }

    BsdSocket_Init();

    /* Apply DNS if configured (for static or dhcp fallback) */
    if (dns) net_stack_set_dns(dns);

    {
        char line[80];
        if (net_stack_dhcp_used()) {
            char ipbuf[20];
            net_ip_to_str(net_stack_get_ip(), ipbuf);
            cmd_scopy(line, "NetStart: network up via DHCP: ", sizeof(line));
            cmd_scat(line, ipbuf, sizeof(line));
            PRINT(line);
        } else if (is_dhcp) {
            /* DHCP requested but we fell back */
            char ipbuf[20];
            net_ip_to_str(ip, ipbuf);
            cmd_scopy(line, "NetStart: network up: ", sizeof(line));
            cmd_scat(line, ipbuf, sizeof(line));
            cmd_scat(line, " (static fallback)", sizeof(line));
            PRINT(line);
        } else {
            char ipbuf[20];
            net_ip_to_str(ip, ipbuf);
            cmd_scopy(line, "NetStart: network up: ", sizeof(line));
            cmd_scat(line, ipbuf, sizeof(line));
            cmd_scat(line, " (static)", sizeof(line));
            PRINT(line);
        }
    }
}
