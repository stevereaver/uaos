/* cmd_fdisk.c — C:fdisk — interactive partition table editor */

#include "cmd_internal.h"

void Cmd_Fdisk(NativeCmdCtx *ctx, const char *args)
{
    /* -l: list available disks */
    if (args && args[0] == '-' && args[1] == 'l') {
        PRINT("Available block devices:");

        BlockDev *dev = BlockDev_GetList();
        int count = 0;
        while (dev) {
            if (dev->part_offset != 0) { dev = dev->next; continue; }

            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "  ", CMD_MAX_LINE);
            cmd_scat(msg, dev->name, CMD_MAX_LINE);
            cmd_scat(msg, " - ", CMD_MAX_LINE);

            uint64_t capacity = BlockDev_GetCapacity(dev);
            uint64_t mb = (capacity * 512ULL) / (1024ULL * 1024ULL);
            cmd_uint_to_dec((uint32_t)mb, msg + cmd_slen(msg),
                            CMD_MAX_LINE - cmd_slen(msg));
            cmd_scat(msg, " MB", CMD_MAX_LINE);
            PRINT(msg);
            dev = dev->next;
            count++;
        }

        if (count == 0) PRINT("  No block devices found.");
        PRINT("");
        PRINT("Usage: fdisk <device>");
        PRINT("       fdisk -l      (list available disks)");
        PRINT("Example: fdisk virtio0");
        return;
    }

    if (!args || !*args) {
        PRINT("Usage: fdisk <device>");
        PRINT("       fdisk -l      (list available disks)");
        PRINT("Example: fdisk virtio0");
        PRINT("");
        PRINT("Note: Partition operations require disk I/O support.");
        return;
    }

    char devname[32];
    int i = 0;
    const char *p = args;
    while (*p && *p != ' ' && i < 31) { devname[i++] = *p++; }
    devname[i] = '\0';

    BlockDev *dev = BlockDev_Find(devname);
    if (!dev) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "Device not found: ", CMD_MAX_LINE);
        cmd_scat(msg, devname, CMD_MAX_LINE);
        PRINT(msg);
        PRINT("");
        PRINT("Use 'fdisk -l' to list available devices.");
        return;
    }

    /* Enter interactive fdisk mode via shell callback */
    if (ctx->set_fdisk_mode) {
        ctx->set_fdisk_mode(ctx->shell_extra, dev);
    } else {
        PRINT("fdisk: interactive mode not available in this context.");
    }
}
