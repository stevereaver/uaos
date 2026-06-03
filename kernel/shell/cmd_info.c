/* cmd_info.c — C:info — show mounted disks and volumes (AmigaDOS style) */

#include "cmd_internal.h"

/* Append src into dst[max], padded with spaces to exactly 'width' chars */
static void pad_field(char *dst, const char *src, int max, int width)
{
    int dl = cmd_slen(dst);
    int si = 0;
    while (si < width && dl < max - 1 && src[si]) {
        dst[dl++] = src[si++];
    }
    while (si++ < width && dl < max - 1) {
        dst[dl++] = ' ';
    }
    dst[dl] = '\0';
}

static void format_cap(uint64_t bytes, char *out, int max)
{
    if (bytes < 1024ULL) {
        cmd_uint_to_dec((uint32_t)bytes, out, max);
        cmd_scat(out, "B", max);
    } else if (bytes < 1024ULL * 1024) {
        cmd_uint_to_dec((uint32_t)(bytes / 1024), out, max);
        cmd_scat(out, "K", max);
    } else if (bytes < 1024ULL * 1024 * 1024) {
        cmd_uint_to_dec((uint32_t)(bytes / (1024ULL * 1024)), out, max);
        cmd_scat(out, "M", max);
    } else {
        cmd_uint_to_dec((uint32_t)(bytes / (1024ULL * 1024 * 1024)), out, max);
        cmd_scat(out, "G", max);
    }
}

void Cmd_Info(NativeCmdCtx *ctx, const char *args)
{
    if (args && *args) {
        /* Info for a specific device */
        char devname[32] = {0};
        int i = 0;
        const char *p = args;
        while (*p && *p != ' ' && i < 31) { devname[i++] = *p++; }
        devname[i] = '\0';

        /* Handle RAM: special case */
        if (cmd_seq(devname, "RAM") || cmd_seq(devname, "RAM:")) {
            PRINT("Unit: RAM:");
            PRINT("Size: Dynamic");
            PRINT("Status: Read/Write");
            return;
        }

        BlockDev *dev = BlockDev_Find(devname);
        if (!dev) {
            BlockDev *all = BlockDev_GetList();
            while (all) {
                if (all->display_name && cmd_seq(all->display_name, devname)) {
                    dev = all; break;
                }
                all = all->next;
            }
        }
        if (!dev) {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "Device not found: ", CMD_MAX_LINE);
            cmd_scat(msg, devname, CMD_MAX_LINE);
            PRINT(msg);
            return;
        }

        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "Unit: ", CMD_MAX_LINE);
        cmd_scat(msg, dev->display_name ? dev->display_name : dev->name, CMD_MAX_LINE);
        PRINT(msg);

        uint64_t cap   = BlockDev_GetCapacity(dev);
        uint64_t bytes = cap * dev->sector_size;
        char sz[16]; sz[0] = '\0';
        format_cap(bytes, sz, 16);

        cmd_scopy(msg, "Size: ", CMD_MAX_LINE);
        cmd_scat(msg, sz, CMD_MAX_LINE);
        PRINT(msg);

        PRINT("Status: Read/Write");
        return;
    }

    /* Show all mounted disks */
    PRINT("Mounted disks:");
    PRINT("Unit      Size       Used       Free      Full  Errs Status        Name");

    BlockDev *dev = BlockDev_GetList();
    while (dev) {
        if (dev->part_offset != 0) {
            uint64_t cap   = BlockDev_GetCapacity(dev);
            uint64_t bytes = cap * dev->sector_size;
            char sz[16]; sz[0] = '\0';
            format_cap(bytes, sz, 16);

            const char *name = dev->display_name ? dev->display_name : dev->name;
            char vol_label[16] = {0};
            BlockDev_ReadVolLabel(dev, vol_label, sizeof(vol_label));
            const char *vol_name = vol_label[0] ? vol_label : name;

            char line[CMD_MAX_LINE];
            line[0] = '\0';
            pad_field(line, name,         CMD_MAX_LINE, 10);
            pad_field(line, sz,           CMD_MAX_LINE, 11);
            pad_field(line, "0",          CMD_MAX_LINE, 11);
            pad_field(line, sz,           CMD_MAX_LINE, 11);
            pad_field(line, "0%",         CMD_MAX_LINE,  6);
            pad_field(line, "0",          CMD_MAX_LINE,  5);
            pad_field(line, "Read/Write", CMD_MAX_LINE, 14);
            pad_field(line, vol_name,     CMD_MAX_LINE, 10);
            PRINT(line);
        }
        dev = dev->next;
    }

    /* RAM: pseudo-entry */
    {
        char line[CMD_MAX_LINE];
        line[0] = '\0';
        pad_field(line, "RAM:",       CMD_MAX_LINE, 10);
        pad_field(line, "Dynamic",    CMD_MAX_LINE, 11);
        pad_field(line, "0",          CMD_MAX_LINE, 11);
        pad_field(line, "\xe2\x80\x94", CMD_MAX_LINE, 11); /* em dash */
        pad_field(line, "0%",         CMD_MAX_LINE,  6);
        pad_field(line, "0",          CMD_MAX_LINE,  5);
        pad_field(line, "Read/Write", CMD_MAX_LINE, 14);
        pad_field(line, "RAM",        CMD_MAX_LINE, 10);
        PRINT(line);
    }

    PRINT("");
    PRINT("Volumes available:");
    int n = VFS_GetMountCount();
    for (int i = 0; i < n; i++) {
        char vol[16];
        if (VFS_GetMountName(i, vol, sizeof(vol))) {
            char line[CMD_MAX_LINE];
            cmd_scopy(line, vol, CMD_MAX_LINE);
            cmd_scat(line, ": [Mounted]", CMD_MAX_LINE);
            PRINT(line);
        }
    }
}
