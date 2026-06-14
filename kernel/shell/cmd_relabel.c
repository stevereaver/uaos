/* cmd_relabel.c — C:relabel — rename a volume */

#include "cmd_internal.h"

void Cmd_Relabel(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: relabel <old_name> <new_name>");
        PRINT("Renames a mounted volume.");
        return;
    }

    const char *p = args;
    while (*p == ' ' || *p == '\t') p++;

    char old_name[16];
    int i = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != ':' && i < 15)
        old_name[i++] = *p++;
    old_name[i] = '\0';

    while (*p == ' ' || *p == '\t') p++;

    char new_name[16];
    i = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != ':' && i < 15)
        new_name[i++] = *p++;
    new_name[i] = '\0';

    if (!old_name[0] || !new_name[0]) {
        PRINT("Usage: relabel <old_name> <new_name>");
        return;
    }

    if (VFS_RenameVol(old_name, new_name) == 0) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "Volume renamed: ", CMD_MAX_LINE);
        cmd_scat(msg, old_name, CMD_MAX_LINE);
        cmd_scat(msg, " -> ", CMD_MAX_LINE);
        cmd_scat(msg, new_name, CMD_MAX_LINE);
        PRINT(msg);
    } else {
        PRINT("Volume not found or rename failed.");
    }
}
