/* cmd_which.c — C:which — locate a command */

#include "cmd_internal.h"

void Cmd_Which(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) { PRINT("Usage: which <command>"); return; }

    /* Extract command name (first token) */
    char cmd[32];
    int i = 0;
    const char *p = args;
    while (*p && *p != ' ' && i < 31) { cmd[i++] = *p++; }
    cmd[i] = '\0';

    char msg[CMD_MAX_LINE];

    /* 1. Check shell builtins via callback */
    if (ctx->is_builtin && ctx->is_builtin(cmd)) {
        cmd_scopy(msg, cmd, CMD_MAX_LINE);
        cmd_scat(msg, " is a shell built-in command", CMD_MAX_LINE);
        PRINT(msg);
        return;
    }

    /* 2. Check native command registry (C: binaries) */
    if (NativeCmd_Exists(cmd)) {
        cmd_scopy(msg, "C:", CMD_MAX_LINE);
        cmd_scat(msg, cmd, CMD_MAX_LINE);
        PRINT(msg);
        return;
    }

    /* 3. Search PATH directories */
    if (ctx->path && *ctx->path) {
        char path_buf[256];
        cmd_scopy(path_buf, ctx->path, 256);
        char *pp = path_buf;
        while (*pp) {
            while (*pp == ' ') pp++;
            if (!*pp) break;
            char entry[64];
            int ei = 0;
            while (*pp && *pp != ' ' && ei < 63) { entry[ei++] = *pp++; }
            entry[ei] = '\0';
            if (ei > 0) {
                char full[128];
                cmd_scopy(full, entry, 128);
                if (ei > 0 && entry[ei-1] != ':' && entry[ei-1] != '/')
                    cmd_scat(full, "/", 128);
                cmd_scat(full, cmd, 128);
                VfsFile test;
                if (VFS_Open(&test, full, VFS_READ)) {
                    VFS_Close(&test);
                    PRINT(full);
                    return;
                }
            }
        }
    }

    /* 4. Check current directory */
    {
        char full[CMD_MAX_PATH];
        cmd_make_abs(ctx->cwd, cmd, full, CMD_MAX_PATH);
        VfsFile test;
        if (VFS_Open(&test, full, VFS_READ)) {
            VFS_Close(&test);
            PRINT(full);
            return;
        }
    }

    cmd_scopy(msg, cmd, CMD_MAX_LINE);
    cmd_scat(msg, " not found", CMD_MAX_LINE);
    PRINT(msg);
}
