/* cmd_pktmon.c — C:pktmon — in-guest pcap capture (UAOS-214)
 *
 *   pktmon START FILE=path [MAX=n]   capture frames to pcap file
 *   pktmon STOP                      close capture
 *   pktmon                           status
 *
 * Works on bare metal (sky2) as well as QEMU — unlike the QEMU
 * filter-dump pcap.  Fetch the file via telnet; open in Wireshark.
 */

#include "cmd_internal.h"
#include "../net/pktmon.h"

void Cmd_Pktmon(NativeCmdCtx *ctx, const char *args)
{
    char line[CMD_MAX_LINE], num[24];

    if (args && cmd_kw_find(args, "stop")) {
        Pktmon_Stop();
        PRINT("pktmon: capture closed");
        return;
    }
    if (args && cmd_kw_find(args, "start")) {
        const char *p = cmd_kv_find(args, "file");
        if (!p) { PRINT("pktmon: START FILE=path [MAX=n]"); return; }
        char path[CMD_MAX_PATH]; int i = 0;
        while (p[i] && p[i] != ' ' && i < CMD_MAX_PATH - 1) { path[i] = p[i]; i++; }
        path[i] = 0;
        uint32_t max = 4096;
        uint64_t v;
        if ((p = cmd_kv_find(args, "max")) && cmd_parse_uint(p, &v)) max = (uint32_t)v;
        if (Pktmon_Start(path, max)) {
            cmd_scopy(line, "pktmon: capturing to ", CMD_MAX_LINE);
            cmd_scat(line, path, CMD_MAX_LINE);
            PRINT(line);
        } else PRINT("pktmon: could not open output file");
        return;
    }

    int open_; uint32_t pkts, dropped; const char *path;
    Pktmon_Status(&open_, &pkts, &dropped, &path);
    cmd_scopy(line, "pktmon: ", CMD_MAX_LINE);
    cmd_scat(line, open_ ? "capturing" : "idle", CMD_MAX_LINE);
    if (open_ && path) { cmd_scat(line, " -> ", CMD_MAX_LINE); cmd_scat(line, path, CMD_MAX_LINE); }
    cmd_scat(line, " pkts=", CMD_MAX_LINE);
    cmd_uint_to_dec(pkts, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " dropped=", CMD_MAX_LINE);
    cmd_uint_to_dec(dropped, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
    PRINT(line);
    PRINT("usage: pktmon START FILE=path [MAX=n] | STOP");
}
