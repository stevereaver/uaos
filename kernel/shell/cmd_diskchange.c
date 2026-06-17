/* cmd_diskchange.c — C:diskchange — signal a disk-change event on a device
 *
 * Syntax: diskchange <device>
 *         diskchange Device=DH0:
 *
 * Informs the filesystem layer that the medium in <device> has been swapped.
 * In practice this re-scans the partition and remounts the volume so that the
 * VFS reflects the new disk contents.
 *
 * Return codes: 0 = success, 5 = device not found / error.
 */

#include "cmd_internal.h"

void Cmd_DiskChange(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: diskchange <device>");
        PRINT("       diskchange Device=DH0:");
        PRINT("");
        PRINT("Signals that the disk in <device> has changed and remounts it.");
        return;
    }

    char devname[32] = {0};

    const char *p = args;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        /* Device=<name> keyword */
        if ((p[0]=='D'||p[0]=='d') && (p[1]=='e'||p[1]=='E') &&
            (p[2]=='v'||p[2]=='V') && (p[3]=='i'||p[3]=='I') &&
            (p[4]=='c'||p[4]=='C') && (p[5]=='e'||p[5]=='E') && p[6]=='=') {
            p += 7;
            int i = 0;
            while (*p && *p != ' ' && i < 31) devname[i++] = *p++;
            devname[i] = '\0';
            continue;
        }

        if (!devname[0]) {
            int i = 0;
            while (*p && *p != ' ' && i < 31) devname[i++] = *p++;
            devname[i] = '\0';
        } else {
            while (*p && *p != ' ') p++;
        }
    }

    if (!devname[0]) {
        PRINT("diskchange: no device specified.");
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 20);
        return;
    }

    /* Find device by display name or raw name */
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
        cmd_scopy(msg, "diskchange: device not found: ", CMD_MAX_LINE);
        cmd_scat(msg, devname, CMD_MAX_LINE);
        PRINT(msg);
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 5);
        return;
    }

    /* Re-check whether the device has a valid filesystem now */
    int formatted = BlockDev_CheckFormatted(dev);
    dev->formatted = formatted;

    char msg[CMD_MAX_LINE];
    cmd_scopy(msg, "diskchange: signalled for ", CMD_MAX_LINE);
    cmd_scat(msg, dev->display_name ? dev->display_name : dev->name, CMD_MAX_LINE);

    if (formatted) {
        /* Attempt to mount / remount the partition.
         * VFS_MountPartition uses the display name (without trailing colon). */
        const char *dname = dev->display_name ? dev->display_name : dev->name;
        /* Strip trailing ':' to get the volume name */
        char volname[32];
        int vi = 0;
        while (dname[vi] && dname[vi] != ':' && vi < 31) {
            volname[vi] = dname[vi];
            vi++;
        }
        volname[vi] = '\0';

        if (volname[0]) {
            int mret = VFS_MountPartition(volname);
            if (mret == 0) {
                cmd_scat(msg, " — remounted as ", CMD_MAX_LINE);
                cmd_scat(msg, volname, CMD_MAX_LINE);
                cmd_scat(msg, ":", CMD_MAX_LINE);
            } else {
                cmd_scat(msg, " — remount skipped (already mounted or error)", CMD_MAX_LINE);
            }
        }
    } else {
        cmd_scat(msg, " — no filesystem found on new medium", CMD_MAX_LINE);
    }

    PRINT(msg);
}
