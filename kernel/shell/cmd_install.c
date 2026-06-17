/* cmd_install.c — C:install — write bootblock to a disk device
 *
 * Syntax:
 *   install <device> [NOBOOT]
 *   install Device=DH0:
 *
 * Writes a minimal UAOS bootblock (standard 1024-byte boot record) to
 * sector 0 of the named block device.  The NOBOOT flag writes a
 * non-bootable marker instead so the disk is recognised but won't auto-boot.
 *
 * Return codes: 0 = success, 5 = device not found, 20 = write error.
 */

#include "cmd_internal.h"

/* Standard UAOS/AmigaDOS bootblock magic ("UAOS") in the first 4 bytes,
 * followed by a checksum word and the root-block pointer.  The rest of
 * the 1024-byte boot record is zeroed — a real stage-1 loader would live
 * here, but for our purposes the magic bytes are sufficient for the
 * firmware to recognise the disk as bootable. */

#define BB_SIZE 1024   /* AmigaDOS bootblock is always 1024 bytes (2 sectors) */

static const uint8_t k_bb_magic[4] = { 'U', 'A', 'O', 'S' };

void Cmd_Install(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: install <device> [NOBOOT]");
        PRINT("       install Device=DH0: [NOBOOT]");
        PRINT("");
        PRINT("Writes a bootblock to sector 0 of <device>.");
        PRINT("NOBOOT marks the disk as non-bootable.");
        return;
    }

    char devname[32] = {0};
    int  noboot = 0;

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

        /* NOBOOT switch */
        if ((p[0]=='N'||p[0]=='n') && (p[1]=='O'||p[1]=='o') &&
            (p[2]=='B'||p[2]=='b') && (p[3]=='O'||p[3]=='o') &&
            (p[4]=='O'||p[4]=='o') && (p[5]=='T'||p[5]=='t') &&
            (p[6]==' ' || p[6]=='\0')) {
            noboot = 1;
            p += 6;
            continue;
        }

        /* Positional argument */
        if (!devname[0]) {
            int i = 0;
            while (*p && *p != ' ' && i < 31) devname[i++] = *p++;
            devname[i] = '\0';
        } else {
            /* Skip unknown token */
            while (*p && *p != ' ') p++;
        }
    }

    if (!devname[0]) {
        PRINT("install: no device specified.");
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
        cmd_scopy(msg, "install: device not found: ", CMD_MAX_LINE);
        cmd_scat(msg, devname, CMD_MAX_LINE);
        PRINT(msg);
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 5);
        return;
    }

    /* Build bootblock buffer (1024 bytes = 2 x 512-byte sectors) */
    static uint8_t bb[BB_SIZE];
    int i;
    for (i = 0; i < BB_SIZE; i++) bb[i] = 0;

    if (!noboot) {
        /* Magic identifier */
        bb[0] = k_bb_magic[0];
        bb[1] = k_bb_magic[1];
        bb[2] = k_bb_magic[2];
        bb[3] = k_bb_magic[3];
        /* Flags byte: 0 = OFS, mark as bootable */
        bb[4] = 0x00;
        bb[5] = 0x00;
        /* Checksum placeholder (words 1–2 reserved) */
        bb[6] = 0x00;
        bb[7] = 0x00;
        /* Root block pointer (default: sector 880 on a standard 80-cyl DD disk) */
        bb[8]  = 0x00;
        bb[9]  = 0x00;
        bb[10] = 0x03;
        bb[11] = 0x70;
    }
    /* NOBOOT: leave all-zero — disk will not be recognised as bootable */

    /* Write two sectors */
    uint32_t sectors_needed = (BB_SIZE + (uint32_t)dev->sector_size - 1) / (uint32_t)dev->sector_size;
    if (sectors_needed == 0) sectors_needed = 2;

    int ret = BlockDev_Write(dev, 0, bb, sectors_needed);
    if (ret != 0) {
        PRINT("install: write failed.");
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 20);
        return;
    }

    char msg[CMD_MAX_LINE];
    if (noboot) {
        cmd_scopy(msg, "install: non-bootable marker written to ", CMD_MAX_LINE);
    } else {
        cmd_scopy(msg, "install: bootblock written to ", CMD_MAX_LINE);
    }
    cmd_scat(msg, dev->display_name ? dev->display_name : dev->name, CMD_MAX_LINE);
    PRINT(msg);
}
