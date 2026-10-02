/* pktmon.c — in-guest packet capture to pcap file (UAOS-214)
 *
 * /tmp/uaos_net.pcap only exists when QEMU's filter-dump is active — on
 * bare metal (sky2 path) there is no host-side capture.  This taps the
 * two universal funnels — netdev_send() for TX and the stack RX callback
 * for RX — and writes a standard libpcap file to a VFS path so it can be
 * pulled over telnet or read after reboot from a disk volume.
 *
 *   C:pktmon start FILE=RAM:pktmon.pcap [MAX=n]
 *   C:pktmon stop
 *   C:pktmon                  status
 *
 * Writes are small (one VFS_Write per record) and happen inline with the
 * network path — this is a debugging tool, not a service; enable only
 * while capturing.
 */

#include "pktmon.h"
#include "../dos/vfs.h"
#include "../boot/kprint.h"
#include <stdint.h>
#include <stddef.h>

extern volatile uint64_t g_pit_ticks;   /* 100 Hz */

static VfsFile  g_cap;
static int      g_open;
static uint32_t g_max_pkts;
static uint32_t g_pkts;
static uint32_t g_dropped;
static char     g_path[64];

static void pw32(VfsFile *f, uint32_t v)
{
    VFS_Write(f, (const uint8_t *)&v, 4);
}

static void pw16(VfsFile *f, uint16_t v)
{
    VFS_Write(f, (const uint8_t *)&v, 2);
}

int Pktmon_Start(const char *path, uint32_t max_packets)
{
    if (g_open) Pktmon_Stop();
    if (!path || !*path) path = "RAM:pktmon.pcap";

    int i = 0;
    while (i < (int)sizeof(g_path) - 1 && path[i]) { g_path[i] = path[i]; i++; }
    g_path[i] = '\0';

    if (!VFS_Open(&g_cap, path, VFS_WRITE | VFS_CREATE | VFS_TRUNC))
        return 0;
    g_open = 1;
    g_pkts = 0;
    g_dropped = 0;
    g_max_pkts = max_packets;

    /* libpcap global header, little-endian, linktype 1 (Ethernet) */
    pw32(&g_cap, 0xA1B2C3D4u);
    pw16(&g_cap, 2);
    pw16(&g_cap, 4);
    pw32(&g_cap, 0);
    pw32(&g_cap, 0);
    pw32(&g_cap, 65535);
    pw32(&g_cap, 1);
    return 1;
}

void Pktmon_Stop(void)
{
    if (g_open) {
        VFS_Close(&g_cap);
        g_open = 0;
        kprint("[pktmon] stopped; ");
        kprinthex(g_pkts);
        kprint(" packets written to ");
        kprint(g_path);
        kprint("\n");
    }
}

void Pktmon_Record(int dir, const uint8_t *frame, uint16_t len)
{
    (void)dir;
    if (!g_open || !frame || !len) return;
    if (g_max_pkts && g_pkts >= g_max_pkts) {
        g_dropped++;
        Pktmon_Stop();
        return;
    }

    /* 100 Hz ticks -> microseconds; per-packet ordering is preserved by
     * the record sequence even when several packets share a tick. */
    uint64_t us = g_pit_ticks * 10000;
    pw32(&g_cap, (uint32_t)(us / 1000000));
    pw32(&g_cap, (uint32_t)(us % 1000000));
    pw32(&g_cap, len);
    pw32(&g_cap, len);
    VFS_Write(&g_cap, frame, len);
    g_pkts++;
}

void Pktmon_Status(int *out_open, uint32_t *out_pkts, uint32_t *out_dropped,
                   const char **out_path)
{
    if (out_open)    *out_open    = g_open;
    if (out_pkts)    *out_pkts    = g_pkts;
    if (out_dropped) *out_dropped = g_dropped;
    if (out_path)    *out_path    = g_path;
}
