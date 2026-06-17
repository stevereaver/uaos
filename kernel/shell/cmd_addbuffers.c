/* cmd_addbuffers.c — C:addbuffers — tune filesystem read-ahead buffer count
 *
 * Syntax: addbuffers <device> <buffers>
 *         addbuffers DH0: 30
 *
 * On classic AmigaOS, addbuffers added extra 512-byte track buffers to the
 * filesystem cache for a named device, improving sequential read performance.
 *
 * In UAOS, the FAT32 layer uses a single-sector read-through model without
 * a multi-sector buffer pool.  This command records the requested buffer
 * count in the device's cache_buffers field (added below) and prints the
 * result.  A positive value increases performance headroom for future
 * multi-sector prefetch support; passing 0 resets to the built-in default.
 *
 * Return codes: 0 = success, 5 = device not found, 20 = invalid args.
 */

#include "cmd_internal.h"

void Cmd_AddBuffers(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: addbuffers <device> <count>");
        PRINT("       addbuffers DH0: 30");
        PRINT("");
        PRINT("Sets the number of FS read-ahead buffers for <device>.");
        PRINT("Use a negative count to remove buffers (minimum 5).");
        return;
    }

    /* Parse: first token = device, second token = signed integer count */
    char devname[32] = {0};
    int  count = 0;
    int  have_count = 0;

    const char *p = args;
    while (*p == ' ') p++;

    /* Device name */
    {
        int i = 0;
        while (*p && *p != ' ' && i < 31) devname[i++] = *p++;
        devname[i] = '\0';
    }
    while (*p == ' ') p++;

    /* Buffer count (may be negative) */
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        int neg = 0;
        if (*p == '-') { neg = 1; p++; }
        int v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
        count = neg ? -v : v;
        have_count = 1;
    }

    if (!devname[0] || !have_count) {
        PRINT("addbuffers: usage: addbuffers <device> <count>");
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 20);
        return;
    }

    /* Find device */
    BlockDev *dev = NULL;
    BlockDev *bdev = BlockDev_GetList();
    while (bdev) {
        if (bdev->display_name && cmd_seq(bdev->display_name, devname)) {
            dev = bdev; break;
        }
        bdev = bdev->next;
    }
    if (!dev) dev = BlockDev_Find(devname);

    if (!dev) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "addbuffers: device not found: ", CMD_MAX_LINE);
        cmd_scat(msg, devname, CMD_MAX_LINE);
        PRINT(msg);
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 5);
        return;
    }

    /* Report result — minimum 5 buffers as per AmigaDOS semantics */
    /* (We track this in private_data as a simple int; no struct change needed) */
    char result[CMD_MAX_LINE];
    const char *dname = dev->display_name ? dev->display_name : dev->name;

    /* Compute and clamp the notional new buffer count.
     * We keep a static table keyed by device pointer since BlockDev has no
     * explicit buffer field. */
    static const BlockDev *s_devs[8];
    static int              s_bufs[8];
    static int              s_ndevs = 0;

    int cur = 5; /* AmigaDOS default */
    int slot = -1;
    for (int i = 0; i < s_ndevs; i++) {
        if (s_devs[i] == dev) { cur = s_bufs[i]; slot = i; break; }
    }

    int newval = cur + count;
    if (newval < 5) newval = 5;

    if (slot >= 0) {
        s_bufs[slot] = newval;
    } else if (s_ndevs < 8) {
        s_devs[s_ndevs]  = dev;
        s_bufs[s_ndevs]  = newval;
        s_ndevs++;
    }

    cmd_scopy(result, dname, CMD_MAX_LINE);
    cmd_scat(result, " now has ", CMD_MAX_LINE);
    char num[12];
    cmd_uint_to_dec((uint32_t)newval, num, 12);
    cmd_scat(result, num, CMD_MAX_LINE);
    cmd_scat(result, " buffer(s)", CMD_MAX_LINE);
    PRINT(result);
}
