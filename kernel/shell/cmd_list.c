/* cmd_list.c — C:list — detailed directory listing */

#include "cmd_internal.h"

void Cmd_List(NativeCmdCtx *ctx, const char *args)
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
    int files = 0, dirs = 0;
    uint32_t total_size = 0;

    while (child) {
        char line[CMD_MAX_LINE];
        line[0] = '\0';

        /* Protection flags */
        char prot[8] = "rwed----";
        if (child->attrs & RAMFS_ATTR_READONLY) {
            prot[0] = '-'; prot[1] = '-';
        }
        if (child->attrs & RAMFS_ATTR_HIDDEN) {
            prot[4] = 'h';
        }

        /* Size */
        char sz[12];
        if (child->type == RAMFS_TYPE_DIR) {
            cmd_scopy(sz, "   <dir>", 12);
        } else {
            cmd_uint_to_dec(child->size, sz, 12);
        }

        /* Build line */
        cmd_scat(line, "  ", CMD_MAX_LINE);
        cmd_scat(line, prot, CMD_MAX_LINE);
        cmd_scat(line, "  ", CMD_MAX_LINE);
        cmd_scat(line, sz, CMD_MAX_LINE);
        cmd_scat(line, "  ", CMD_MAX_LINE);
        cmd_scat(line, child->name, CMD_MAX_LINE);

        /* Comment */
        if (child->comment[0]) {
            cmd_scat(line, "  (", CMD_MAX_LINE);
            cmd_scat(line, child->comment, CMD_MAX_LINE);
            cmd_scat(line, ")", CMD_MAX_LINE);
        }

        PRINT(line);
        count++;
        if (child->type == RAMFS_TYPE_DIR) dirs++;
        else { files++; total_size += child->size; }
        child = child->next_sibling;
    }

    PRINT("");
    char summary[CMD_MAX_LINE];
    char cn[8];
    cmd_uint_to_dec((uint32_t)count, cn, 8);
    cmd_scopy(summary, cn, CMD_MAX_LINE);
    cmd_scat(summary, " entries (", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)files, cn, 8);
    cmd_scat(summary, cn, CMD_MAX_LINE);
    cmd_scat(summary, " files, ", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)dirs, cn, 8);
    cmd_scat(summary, cn, CMD_MAX_LINE);
    cmd_scat(summary, " dirs)", CMD_MAX_LINE);
    PRINT(summary);

    char ts[CMD_MAX_LINE];
    cmd_scopy(ts, "Total bytes: ", CMD_MAX_LINE);
    cmd_uint_to_dec(total_size, cn, 8);
    cmd_scat(ts, cn, CMD_MAX_LINE);
    PRINT(ts);
}
