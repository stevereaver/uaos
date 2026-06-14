/* cmd_execute.c — C:execute — run a script file line by line */

#include "cmd_internal.h"

#define MAX_SCRIPT_SIZE 4096
static char g_exec_buf[MAX_SCRIPT_SIZE];

void Cmd_Execute(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: execute <script>");
        PRINT("Executes a script file line by line.");
        return;
    }

    char path[CMD_MAX_PATH];
    cmd_make_abs(ctx->cwd, args, path, CMD_MAX_PATH);

    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ)) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "Cannot open: ", CMD_MAX_LINE);
        cmd_scat(msg, path, CMD_MAX_LINE);
        PRINT(msg);
        return;
    }

    uint32_t size = VFS_Size(&fh);
    if (size == 0 || size >= MAX_SCRIPT_SIZE) {
        PRINT("Script empty or too large (max 4KB)");
        VFS_Close(&fh);
        return;
    }

    uint32_t nread = VFS_Read(&fh, (uint8_t *)g_exec_buf, size);
    g_exec_buf[nread] = '\0';
    VFS_Close(&fh);

    if (ctx->run_script && ctx->shell_extra) {
        ctx->run_script(ctx->shell_extra, g_exec_buf);
    } else {
        /* Fallback: line-by-line dispatch without flow control */
        char line[CMD_MAX_LINE];
        const char *p = g_exec_buf;
        while (*p) {
            int li = 0;
            while (*p && *p != '\n' && li < CMD_MAX_LINE - 1) {
                if (*p != '\r') line[li++] = *p;
                p++;
            }
            if (*p == '\n') p++;
            line[li] = '\0';

            const char *lp = line;
            while (*lp == ' ' || *lp == '\t') lp++;
            if (!*lp || *lp == ';') continue;

            if (ctx->dispatch_line && ctx->shell_extra)
                ctx->dispatch_line(ctx->shell_extra, line);
            else
                PRINT(line);
        }
    }
}
