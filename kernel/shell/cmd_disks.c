/* cmd_disks.c — C:disks — list detected block devices */

#include "cmd_internal.h"

void Cmd_Disks(NativeCmdCtx *ctx, const char *args)
{
    (void)args;

    BlockDev *dev = BlockDev_GetList();
    if (!dev) {
        PRINT("No block devices detected");
        return;
    }

    /* Only show whole-disk devices (part_offset == 0) */
    int count = 0;
    while (dev) {
        if (dev->part_offset == 0) {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "Device: ", CMD_MAX_LINE);
            cmd_scat(msg, dev->name, CMD_MAX_LINE);
            PRINT(msg);

            uint64_t capacity = BlockDev_GetCapacity(dev);
            uint64_t mb = (capacity * 512ULL) / (1024ULL * 1024ULL);

            cmd_scopy(msg, "  Capacity: ", CMD_MAX_LINE);
            cmd_uint_to_dec((uint32_t)capacity, msg + cmd_slen(msg),
                            CMD_MAX_LINE - cmd_slen(msg));
            cmd_scat(msg, " sectors (", CMD_MAX_LINE);
            cmd_uint_to_dec((uint32_t)mb, msg + cmd_slen(msg),
                            CMD_MAX_LINE - cmd_slen(msg));
            cmd_scat(msg, " MB)", CMD_MAX_LINE);
            PRINT(msg);

            cmd_scopy(msg, "  Sector size: ", CMD_MAX_LINE);
            cmd_uint_to_dec(dev->sector_size, msg + cmd_slen(msg),
                            CMD_MAX_LINE - cmd_slen(msg));
            cmd_scat(msg, " bytes", CMD_MAX_LINE);
            PRINT(msg);

            count++;
        }
        dev = dev->next;
    }

    if (count == 0) {
        PRINT("No whole-disk devices detected");
    }
}
