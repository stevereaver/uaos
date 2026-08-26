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

        /* Handle logical volume name (e.g., RAM:, Workbench:) */
        {
            char vol_path[48];
            cmd_scopy(vol_path, devname, sizeof(vol_path));
            if (cmd_slen(vol_path) > 0 && vol_path[cmd_slen(vol_path) - 1] != ':')
                cmd_scat(vol_path, ":", sizeof(vol_path));

            uint32_t total = 0, used = 0;
            if (VFS_GetVolumeInfo(vol_path, &total, &used) == 0) {
                uint32_t free = (total > used) ? (total - used) : 0;
                char sz[16], usz[16], fsz[16];
                sz[0] = usz[0] = fsz[0] = '\0';
                format_cap(total, sz, 16);
                format_cap(used, usz, 16);
                format_cap(free, fsz, 16);

                char msg[CMD_MAX_LINE];
                cmd_scopy(msg, "Unit: ", CMD_MAX_LINE);
                cmd_scat(msg, vol_path, CMD_MAX_LINE); PRINT(msg);
                cmd_scopy(msg, "Size: ", CMD_MAX_LINE);
                cmd_scat(msg, sz, CMD_MAX_LINE); PRINT(msg);
                cmd_scopy(msg, "Used: ", CMD_MAX_LINE);
                cmd_scat(msg, usz, CMD_MAX_LINE); PRINT(msg);
                cmd_scopy(msg, "Free: ", CMD_MAX_LINE);
                cmd_scat(msg, fsz, CMD_MAX_LINE); PRINT(msg);
                cmd_scopy(msg, "Status: Read/Write", CMD_MAX_LINE); PRINT(msg);
                return;
            }
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

    /* RAM: entry */
    {
        uint32_t total = 0, used = 0;
        VFS_GetVolumeInfo("RAM:", &total, &used);
        uint32_t free = (total > used) ? (total - used) : 0;
        int full_pct = (total > 0) ? (int)((used * 100ULL) / total) : 0;

        char sz[16], usz[16], fsz[16], pct[8];
        sz[0] = usz[0] = fsz[0] = pct[0] = '\0';
        format_cap(total, sz, 16);
        format_cap(used, usz, 16);
        format_cap(free, fsz, 16);
        cmd_uint_to_dec((uint32_t)full_pct, pct, 8);
        cmd_scat(pct, "%", 8);

        char line[CMD_MAX_LINE];
        line[0] = '\0';
        pad_field(line, "RAM:",       CMD_MAX_LINE, 10);
        pad_field(line, sz,           CMD_MAX_LINE, 11);
        pad_field(line, usz,          CMD_MAX_LINE, 11);
        pad_field(line, fsz,          CMD_MAX_LINE, 11);
        pad_field(line, pct,          CMD_MAX_LINE,  6);
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
