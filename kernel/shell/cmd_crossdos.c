/* cmd_crossdos.c — C:crossdos — mount PC-format (FAT12/16) media
 *
 * Usage: crossdos [DEVICE] [VOLUME]
 *   crossdos DF0: PC0:    — mount floppy DF0: as PC0:
 *   crossdos list          — list CrossDOS volumes
 *   crossdos unmount PC0:  — unmount a CrossDOS volume
 *
 * If no args, probes DF0: and mounts as PC0: if valid FAT12/16.
 */

#include "cmd_internal.h"
#include "../dos/crossdos_handler.h"
#include "../dos/dos_list.h"

void Cmd_CrossDOS(NativeCmdCtx *ctx, const char *args)
{
    /* Parse args */
    if (!args || !args[0]) {
        /* Default: probe DF0: and mount as PC0: */
        BlockDev *bdev = BlockDev_Find("floppy0");
        if (!bdev) {
            PRINT("crossdos: no floppy device (DF0:) found");
            return;
        }
        if (!CrossDOS_Probe(bdev)) {
            PRINT("crossdos: DF0: does not contain a valid FAT12/16 volume");
            return;
        }
        Handler *h = CrossDOSHandler_Create("PC0", bdev);
        if (!h) {
            PRINT("crossdos: failed to create handler");
            return;
        }
        DosList_AddDevice("PC0", &h->port, ID_DOS_DISK);
        PRINT("crossdos: PC0: mounted (FAT12/16)");
        return;
    }

    /* Check for "list" */
    if (cmd_seq_ci(args, "list")) {
        int count = DosList_Count();
        if (count == 0) {
            PRINT("crossdos: no devices mounted");
            return;
        }
        DosList *prev = NULL;
        char msg[CMD_MAX_LINE];
        while ((prev = DosList_Next(prev)) != NULL) {
            /* Only list PC-format volumes (name starts with PC) */
            const char *name = prev->dol_Name ? (const char *)(uintptr_t)prev->dol_Name : "";
            if (name[0] == 'P' && name[1] == 'C') {
                cmd_scopy(msg, "  ", CMD_MAX_LINE);
                cmd_scat(msg, name, CMD_MAX_LINE);
                cmd_scat(msg, ":", CMD_MAX_LINE);
                PRINT(msg);
            }
        }
        return;
    }

    /* Check for "unmount" */
    if (cmd_kw_find(args, "unmount")) {
        /* Extract volume name */
        const char *p = args;
        while (*p == ' ') p++;
        /* Skip "unmount" */
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        if (!*p) {
            PRINT("crossdos: unmount requires a volume name");
            return;
        }
        /* Strip trailing colon */
        char vol[16];
        int i = 0;
        while (p[i] && p[i] != ':' && p[i] != ' ' && i < 15) {
            vol[i] = p[i]; i++;
        }
        vol[i] = '\0';
        DosList_Remove(vol, 0);  /* type 0 = any */
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "crossdos: ", CMD_MAX_LINE);
        cmd_scat(msg, vol, CMD_MAX_LINE);
        cmd_scat(msg, ": unmounted", CMD_MAX_LINE);
        PRINT(msg);
        return;
    }

    /* Parse "DEVICE VOLUME" format */
    char dev_name[16];
    char vol_name[16];
    int di = 0, vi = 0;
    const char *p = args;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && *p != ':' && di < 15) dev_name[di++] = *p++;
    dev_name[di] = '\0';
    if (*p == ':') p++;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && *p != ':' && vi < 15) vol_name[vi++] = *p++;
    vol_name[vi] = '\0';

    if (!di || !vi) {
        PRINT("Usage: crossdos [DEVICE VOLUME | list | unmount VOL:]");
        return;
    }

    /* Find block device */
    BlockDev *bdev = BlockDev_Find(dev_name);
    if (!bdev) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "crossdos: device ", CMD_MAX_LINE);
        cmd_scat(msg, dev_name, CMD_MAX_LINE);
        cmd_scat(msg, " not found", CMD_MAX_LINE);
        PRINT(msg);
        return;
    }

    if (!CrossDOS_Probe(bdev)) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "crossdos: ", CMD_MAX_LINE);
        cmd_scat(msg, dev_name, CMD_MAX_LINE);
        cmd_scat(msg, ": not a valid FAT12/16 volume", CMD_MAX_LINE);
        PRINT(msg);
        return;
    }

    Handler *h = CrossDOSHandler_Create(vol_name, bdev);
    if (!h) {
        PRINT("crossdos: failed to create handler");
        return;
    }

    DosList_AddDevice(vol_name, &h->port, ID_DOS_DISK);

    char msg[CMD_MAX_LINE];
    cmd_scopy(msg, "crossdos: ", CMD_MAX_LINE);
    cmd_scat(msg, vol_name, CMD_MAX_LINE);
    cmd_scat(msg, ": mounted (FAT12/16)", CMD_MAX_LINE);
    PRINT(msg);
}
