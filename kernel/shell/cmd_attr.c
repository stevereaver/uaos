/* cmd_attr.c — C:attr — display file/directory attributes */

#include "cmd_internal.h"

void Cmd_Attr(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) { PRINT("Usage: attr <path>"); return; }

    char path[CMD_MAX_PATH];
    int i = 0;
    const char *p = args;
    while (*p && *p != ' ' && i < CMD_MAX_PATH - 1) { path[i++] = *p++; }
    path[i] = '\0';

    char abs_path[CMD_MAX_PATH];
    cmd_make_abs(ctx->cwd, path, abs_path, CMD_MAX_PATH);

    uint8_t attrs = VFS_GetAttrs(abs_path);
    if (attrs == 0) {
        VfsFile test;
        if (!VFS_Open(&test, abs_path, VFS_READ)) {
            RamFsNode *dir = VFS_ResolveDir(abs_path);
            if (!dir) {
                char msg[CMD_MAX_LINE];
                cmd_scopy(msg, "Not found: ", CMD_MAX_LINE);
                cmd_scat(msg, abs_path, CMD_MAX_LINE);
                PRINT(msg);
                return;
            }
            attrs = RamFS_GetAttrs(dir);
        } else {
            VFS_Close(&test);
        }
    }

    char msg[CMD_MAX_LINE];
    cmd_scopy(msg, "Attributes: ", CMD_MAX_LINE);
    if (attrs & RAMFS_ATTR_READONLY) cmd_scat(msg, "Read-Only ", CMD_MAX_LINE);
    if (attrs & RAMFS_ATTR_HIDDEN)   cmd_scat(msg, "Hidden ",    CMD_MAX_LINE);
    if (attrs == 0)                  cmd_scat(msg, "None",       CMD_MAX_LINE);
    PRINT(msg);
}
