/* cmd_dir.c — C:dir — list a directory */

#include "cmd_internal.h"

void Cmd_Dir(NativeCmdCtx *ctx, const char *args)
{
    char path[CMD_MAX_PATH];
    if (args && *args)
        cmd_make_abs(ctx->cwd, args, path, CMD_MAX_PATH);
    else
        cmd_scopy(path, ctx->cwd, CMD_MAX_PATH);

    RamFsNode *child = VFS_OpenDir(path);

    char hdr[CMD_MAX_LINE];
    cmd_scopy(hdr, "Directory of ", CMD_MAX_LINE);
    cmd_scat(hdr, path, CMD_MAX_LINE);
    PRINT(hdr);
    PRINT("");

    if (!child) {
        PRINT("  (empty or not found)");
        PRINT("");
        return;
    }

    int count = 0;
    while (child) {
        char line[CMD_MAX_LINE];
        if (child->type == RAMFS_TYPE_DIR) {
            cmd_scopy(line, "  ", CMD_MAX_LINE);
            cmd_scat(line, child->name, CMD_MAX_LINE);
            cmd_scat(line, "  (dir)", CMD_MAX_LINE);
        } else {
            char sz[12];
            cmd_uint_to_dec(child->size, sz, 12);
            cmd_scopy(line, "  ", CMD_MAX_LINE);
            cmd_scat(line, child->name, CMD_MAX_LINE);
            cmd_scat(line, "  ", CMD_MAX_LINE);
            cmd_scat(line, sz, CMD_MAX_LINE);
            cmd_scat(line, " bytes", CMD_MAX_LINE);
        }
        PRINT(line);
        count++;
        child = child->next_sibling;
    }

    PRINT("");
    char summary[CMD_MAX_LINE];
    char cn[8];
    cmd_uint_to_dec((uint32_t)count, cn, 8);
    cmd_scopy(summary, cn, CMD_MAX_LINE);
    cmd_scat(summary, " item(s)", CMD_MAX_LINE);
    PRINT(summary);
}
