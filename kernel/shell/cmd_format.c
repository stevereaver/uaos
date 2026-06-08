/* cmd_format.c — C:format — format a partition */

#include "cmd_internal.h"
#include "../dos/fat32.h"
#include "../dos/vfs.h"

void Cmd_Format(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: format <device> [filesystem]");
        PRINT("       format Device=DH0: Name=Workbench FFS");
        PRINT("");
        PRINT("Supported filesystems: fat32");
        PRINT("");
        PRINT("Note: Format a partition (e.g. virtio01 or DH0:),");
        PRINT("      not the whole disk (virtio0).");
        return;
    }

    /* Parse Amiga-style keyword parameters */
    char devname[32] = {0};
    char volname[12] = {0};
    char fs[16]      = {0};

    const char *p = args;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        if ((p[0]=='D'||p[0]=='d') && (p[1]=='e'||p[1]=='E') &&
            (p[2]=='V'||p[2]=='v') && p[3]=='i' && p[4]=='c' &&
            p[5]=='e' && p[6]=='=') {
            p += 7;
            int i = 0;
            while (*p && *p != ' ' && i < 31) { devname[i++] = *p++; }
            devname[i] = '\0';
        } else if ((p[0]=='N'||p[0]=='n') && (p[1]=='a'||p[1]=='A') &&
                   (p[2]=='M'||p[2]=='m') && (p[3]=='e'||p[3]=='E') && p[4]=='=') {
            p += 5;
            int i = 0;
            while (*p && *p != ' ' && i < 11) { volname[i++] = *p++; }
            volname[i] = '\0';
        } else if ((p[0]=='F'||p[0]=='f') && (p[1]=='F'||p[1]=='f') &&
                   (p[2]=='S'||p[2]=='s')) {
            cmd_scopy(fs, "fat32", 16);
            p += 3;
        } else {
            char tok[32];
            int i = 0;
            while (*p && *p != ' ' && i < 31) { tok[i++] = *p++; }
            tok[i] = '\0';
            if (!devname[0]) cmd_scopy(devname, tok, 32);
            else if (!fs[0]) cmd_scopy(fs, tok, 16);
        }
    }

    if (!fs[0]) cmd_scopy(fs, "fat32", 16);

    if (!devname[0]) {
        PRINT("No device specified.");
        PRINT("Example: format virtio01 fat32");
        PRINT("         format Device=DH0: Name=Workbench FFS");
        return;
    }

    /* Find device — try display_name first, then device name */
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
        cmd_scopy(msg, "Device not found: ", CMD_MAX_LINE);
        cmd_scat(msg, devname, CMD_MAX_LINE);
        PRINT(msg);
        return;
    }

    /* Refuse to format a whole disk */
    if (dev->part_offset == 0) {
        int len = cmd_slen(devname);
        if (len > 0 && !(devname[len-1] >= '0' && devname[len-1] <= '9')) {
            PRINT("Cannot format whole disk.");
            PRINT("Use fdisk to create a partition, then format it.");
            PRINT("Example: format virtio01 fat32");
            return;
        }
    }

    char msg[CMD_MAX_LINE];
    cmd_scopy(msg, "format: ", CMD_MAX_LINE);
    cmd_scat(msg, dev->display_name ? dev->display_name : dev->name, CMD_MAX_LINE);
    if (volname[0]) {
        cmd_scat(msg, " name=", CMD_MAX_LINE);
        cmd_scat(msg, volname, CMD_MAX_LINE);
    }
    cmd_scat(msg, " as ", CMD_MAX_LINE);
    cmd_scat(msg, fs, CMD_MAX_LINE);
    PRINT(msg);
    PRINT("");

    if (cmd_seq(fs, "fat32")) {
        PRINT("Formatting... please wait.");
        int ret = FAT32_Format(dev, volname[0] ? volname : (void*)0);
        if (ret == 0) {
            dev->formatted = 1;
            PRINT("Format complete.");
            /* Auto-mount in VFS */
            const char *dname = dev->display_name ? dev->display_name : dev->name;
            char mnt_name[16];
            int ni = 0, si = 0;
            while (si < 15 && dname[si] && dname[si] != ':')
                mnt_name[ni++] = dname[si++];
            mnt_name[ni] = '\0';

            /* Mount by volume label if provided, else fall back to device name */
            const char *vol_mnt = volname[0] ? volname : mnt_name;
            if (vol_mnt[0]) {
                if (VFS_MountPartition(vol_mnt) == 0) {
                    char msg2[CMD_MAX_LINE];
                    cmd_scopy(msg2, "Mounted as ", CMD_MAX_LINE);
                    cmd_scat(msg2, vol_mnt, CMD_MAX_LINE);
                    cmd_scat(msg2, ":", CMD_MAX_LINE);
                    PRINT(msg2);
                }
            }
        } else {
            PRINT("Format failed.");
        }
    } else {
        PRINT("Unsupported filesystem.");
        PRINT("Supported: fat32");
    }
}
