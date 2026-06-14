/* cmd_filenote.c — C:filenote — set/show file comment string */

#include "cmd_internal.h"

void Cmd_Filenote(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: filenote <file> [comment]");
        PRINT("  Without comment: show current filenote.");
        PRINT("  With comment:    set filenote.");
        return;
    }

    /* Extract file path (first argument) */
    char path[CMD_MAX_PATH];
    const char *p = args;
    while (*p == ' ' || *p == '\t') p++;

    int i = 0;
    while (*p && *p != ' ' && *p != '\t' && i < CMD_MAX_PATH - 1)
        path[i++] = *p++;
    path[i] = '\0';

    while (*p == ' ' || *p == '\t') p++;

    char abs_path[CMD_MAX_PATH];
    cmd_make_abs(ctx->cwd, path, abs_path, CMD_MAX_PATH);

    if (*p) {
        /* Set comment */
        if (VFS_SetComment(abs_path, p) == 0) {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "Filenote set: ", CMD_MAX_LINE);
            cmd_scat(msg, abs_path, CMD_MAX_LINE);
            PRINT(msg);
        } else {
            PRINT("Failed to set filenote.");
        }
    } else {
        /* Show comment */
        char comment[64];
        if (VFS_GetComment(abs_path, comment, sizeof(comment)) == 0 && comment[0]) {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "Filenote for ", CMD_MAX_LINE);
            cmd_scat(msg, abs_path, CMD_MAX_LINE);
            cmd_scat(msg, ": ", CMD_MAX_LINE);
            cmd_scat(msg, comment, CMD_MAX_LINE);
            PRINT(msg);
        } else {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "No filenote for ", CMD_MAX_LINE);
            cmd_scat(msg, abs_path, CMD_MAX_LINE);
            PRINT(msg);
        }
    }
}
