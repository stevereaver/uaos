/* cmd_dismount.c — C:dismount — unmount a filesystem volume
 *
 * Syntax: dismount <volume>
 *         dismount Volume=DH0:
 *
 * Removes the named volume from the VFS mount table: flushes the
 * filesystem, destroys its packet handler, and drops the mount entry.
 * The volume must be a block-device filesystem (FFS or FAT32), must not
 * be the SYS: boot volume, and must have no open files or locks.
 * Remount it afterwards with "diskchange <device>".
 *
 * Return codes: 0 = success, 10 = volume busy, 20 = other error.
 */

#include "cmd_internal.h"

void Cmd_Dismount(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: dismount <volume>");
        PRINT("       dismount Volume=DH0:");
        PRINT("");
        PRINT("Unmounts <volume> (unit name or volume label) after checking");
        PRINT("it has no open files or locks.");
        return;
    }

    char volname[32] = {0};

    const char *p = args;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        /* Volume=<name> keyword */
        if ((p[0]=='V'||p[0]=='v') && (p[1]=='o'||p[1]=='O') &&
            (p[2]=='l'||p[2]=='L') && (p[3]=='u'||p[3]=='U') &&
            (p[4]=='m'||p[4]=='M') && (p[5]=='e'||p[5]=='E') && p[6]=='=') {
            p += 7;
            int i = 0;
            while (*p && *p != ' ' && i < 31) volname[i++] = *p++;
            volname[i] = '\0';
            continue;
        }

        if (!volname[0]) {
            int i = 0;
            while (*p && *p != ' ' && i < 31) volname[i++] = *p++;
            volname[i] = '\0';
        } else {
            while (*p && *p != ' ') p++;
        }
    }

    if (!volname[0]) {
        PRINT("dismount: no volume specified.");
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 20);
        return;
    }

    int r = VFS_Unmount(volname);
    char msg[CMD_MAX_LINE];

    switch (r) {
    case VFS_UNMOUNT_OK:
        cmd_scopy(msg, "dismount: ", CMD_MAX_LINE);
        cmd_scat(msg, volname, CMD_MAX_LINE);
        cmd_scat(msg, " unmounted", CMD_MAX_LINE);
        PRINT(msg);
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 0);
        break;
    case VFS_UNMOUNT_BUSY:
        cmd_scopy(msg, "dismount: ", CMD_MAX_LINE);
        cmd_scat(msg, volname, CMD_MAX_LINE);
        cmd_scat(msg, " is in use (open files or locks)", CMD_MAX_LINE);
        PRINT(msg);
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 10);
        break;
    case VFS_UNMOUNT_SYSVOL:
        cmd_scopy(msg, "dismount: ", CMD_MAX_LINE);
        cmd_scat(msg, volname, CMD_MAX_LINE);
        cmd_scat(msg, " is the system volume - refusing", CMD_MAX_LINE);
        PRINT(msg);
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 20);
        break;
    case VFS_UNMOUNT_NOTSUPP:
        cmd_scopy(msg, "dismount: ", CMD_MAX_LINE);
        cmd_scat(msg, volname, CMD_MAX_LINE);
        cmd_scat(msg, " cannot be dismounted (not a disk volume)", CMD_MAX_LINE);
        PRINT(msg);
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 20);
        break;
    case VFS_UNMOUNT_NOTFOUND:
    default:
        cmd_scopy(msg, "dismount: volume not found: ", CMD_MAX_LINE);
        cmd_scat(msg, volname, CMD_MAX_LINE);
        PRINT(msg);
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 20);
        break;
    }
}
