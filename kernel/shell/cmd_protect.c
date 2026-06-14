/* cmd_protect.c — C:protect — set file protection attributes */

#include "cmd_internal.h"

static int protect_bit(char c)
{
    switch (c) {
        case 'h': case 'H': return FIBF_HOLD;
        case 's': case 'S': return FIBF_SCRIPT;
        case 'p': case 'P': return FIBF_PURE;
        case 'a': case 'A': return FIBF_ARCHIVE;
        case 'r': case 'R': return FIBF_READ;
        case 'w': case 'W': return FIBF_WRITE;
        case 'e': case 'E': return FIBF_EXECUTE;
        case 'd': case 'D': return FIBF_DELETE;
    }
    return 0;
}

static const char *protect_name(int bit)
{
    switch (bit) {
        case FIBF_HOLD:     return "hold";
        case FIBF_SCRIPT:   return "script";
        case FIBF_PURE:     return "pure";
        case FIBF_ARCHIVE:  return "archive";
        case FIBF_READ:     return "read";
        case FIBF_WRITE:    return "write";
        case FIBF_EXECUTE:  return "execute";
        case FIBF_DELETE:   return "delete";
    }
    return "";
}

static void protect_one(NativeCmdCtx *ctx, const char *path,
                        uint16_t set_bits, uint16_t clear_bits,
                        int quiet)
{
    uint16_t current = VFS_GetProtection(path);
    uint16_t final = (current & ~clear_bits) | set_bits;

    if (VFS_SetProtection(path, final) == 0) {
        if (!quiet) {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "Protected: ", CMD_MAX_LINE);
            cmd_scat(msg, path, CMD_MAX_LINE);
            PRINT(msg);
        }
    } else {
        if (!quiet) {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "Failed: ", CMD_MAX_LINE);
            cmd_scat(msg, path, CMD_MAX_LINE);
            PRINT(msg);
        }
    }
}

static void protect_dir(NativeCmdCtx *ctx, const char *path,
                        uint16_t set_bits, uint16_t clear_bits,
                        int quiet)
{
    protect_one(ctx, path, set_bits, clear_bits, quiet);
    RamFsNode *dir = VFS_ResolveDir(path);
    if (!dir) return;
    RamFsNode *child = dir->first_child;
    while (child) {
        char sub[CMD_MAX_PATH];
        cmd_scopy(sub, path, CMD_MAX_PATH);
        int sl = cmd_slen(sub);
        if (sl > 0 && sub[sl - 1] != ':' && sub[sl - 1] != '/') {
            if (sl < CMD_MAX_PATH - 1) { sub[sl] = '/'; sub[sl + 1] = '\0'; }
        }
        cmd_scat(sub, child->name, CMD_MAX_PATH);
        if (child->type == RAMFS_TYPE_DIR) {
            protect_dir(ctx, sub, set_bits, clear_bits, quiet);
        } else {
            protect_one(ctx, sub, set_bits, clear_bits, quiet);
        }
        child = child->next_sibling;
    }
}

void Cmd_Protect(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: protect <file> [+|-][hsparwed] [ALL]");
        return;
    }

    int all   = cmd_kw_find(args, "ALL");
    int quiet = cmd_kw_find(args, "QUIET");

    /* Extract flag operations and path */
    uint16_t set_bits = 0, clear_bits = 0;
    const char *p = args;
    while (*p && (*p == '+' || *p == '-')) {
        char op = *p++;
        char flag = *p++;
        int bit = protect_bit(flag);
        if (bit) {
            if (op == '+') set_bits |= (uint16_t)bit;
            else           clear_bits |= (uint16_t)bit;
        }
        while (*p == ' ') p++;
    }

    while (*p == ' ') p++;

    /* Skip ALL / QUIET keywords to get path */
    char path[CMD_MAX_PATH];
    {
        char clean[CMD_MAX_LINE];
        cmd_kw_strip(p, "ALL", NULL, clean, CMD_MAX_LINE);
        cmd_kw_strip(clean, "QUIET", NULL, clean, CMD_MAX_LINE);
        cmd_make_abs(ctx->cwd, clean, path, CMD_MAX_PATH);
    }

    if (!path[0]) {
        PRINT("Usage: protect <file> [+|-][hsparwed] [ALL]");
        return;
    }

    if (all) {
        protect_dir(ctx, path, set_bits, clear_bits, quiet);
    } else {
        protect_one(ctx, path, set_bits, clear_bits, quiet);
    }
}
