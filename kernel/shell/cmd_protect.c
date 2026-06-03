/* cmd_protect.c — C:protect — set file protection attributes */

#include "cmd_internal.h"

void Cmd_Protect(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: protect [+r|-r][+h|-h] <path>");
        return;
    }

    uint8_t new_attrs  = 0;
    uint8_t clear_mask = 0;
    const char *p = args;

    while (*p && (*p == '+' || *p == '-')) {
        char op   = *p++;
        char flag = *p++;
        if (flag == 'r') {
            if (op == '+') new_attrs  |= RAMFS_ATTR_READONLY;
            else           clear_mask |= RAMFS_ATTR_READONLY;
        } else if (flag == 'h') {
            if (op == '+') new_attrs  |= RAMFS_ATTR_HIDDEN;
            else           clear_mask |= RAMFS_ATTR_HIDDEN;
        } else {
            PRINT("Invalid flag. Use: +r, -r, +h, -h");
            return;
        }
        while (*p == ' ') p++;
    }

    while (*p == ' ') p++;
    if (!*p) { PRINT("Usage: protect [+r|-r][+h|-h] <path>"); return; }

    char path[CMD_MAX_PATH];
    int i = 0;
    while (*p && i < CMD_MAX_PATH - 1) { path[i++] = *p++; }
    path[i] = '\0';

    char abs_path[CMD_MAX_PATH];
    cmd_make_abs(ctx->cwd, path, abs_path, CMD_MAX_PATH);

    uint8_t current = VFS_GetAttrs(abs_path);
    if (current == 0 && VFS_ResolveDir(abs_path) == NULL) {
        VfsFile test;
        if (!VFS_Open(&test, abs_path, VFS_READ)) {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "File not found: ", CMD_MAX_LINE);
            cmd_scat(msg, abs_path, CMD_MAX_LINE);
            PRINT(msg);
            return;
        }
        VFS_Close(&test);
    }

    uint8_t final = (current & ~clear_mask) | new_attrs;
    if (VFS_SetAttrs(abs_path, final) == 0) {
        PRINT("Attributes updated");
    } else {
        PRINT("Failed to set attributes");
    }
}
