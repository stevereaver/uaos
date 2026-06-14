/* cmd_attr.c — C:attr — display file/directory attributes */

#include "cmd_internal.h"

static void prot_bit_str(char *buf, uint16_t prot)
{
    buf[0] = (prot & FIBF_HOLD)    ? 'h' : '-';
    buf[1] = (prot & FIBF_SCRIPT)  ? 's' : '-';
    buf[2] = (prot & FIBF_PURE)    ? 'p' : '-';
    buf[3] = (prot & FIBF_ARCHIVE) ? 'a' : '-';
    buf[4] = (prot & FIBF_READ)    ? '-' : 'r'; /* inverted: bit set = denied */
    buf[5] = (prot & FIBF_WRITE)   ? '-' : 'w';
    buf[6] = (prot & FIBF_EXECUTE) ? '-' : 'e';
    buf[7] = (prot & FIBF_DELETE)  ? '-' : 'd';
    buf[8] = '\0';
}

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

    uint16_t prot = VFS_GetProtection(abs_path);
    char pstr[16];
    prot_bit_str(pstr, prot);

    char msg[CMD_MAX_LINE];
    cmd_scopy(msg, "Attributes: ", CMD_MAX_LINE);
    if (attrs & RAMFS_ATTR_READONLY) cmd_scat(msg, "Read-Only ", CMD_MAX_LINE);
    if (attrs & RAMFS_ATTR_HIDDEN)   cmd_scat(msg, "Hidden ",    CMD_MAX_LINE);
    if (attrs == 0)                  cmd_scat(msg, "Normal ",    CMD_MAX_LINE);
    cmd_scat(msg, " Protection: ", CMD_MAX_LINE);
    cmd_scat(msg, pstr, CMD_MAX_LINE);
    PRINT(msg);
}
