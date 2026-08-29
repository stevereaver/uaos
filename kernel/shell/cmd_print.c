/* cmd_print.c — C:print — send a file to PRT: (parallel port)
 *
 * Usage: print FILE
 * Opens the named file and sends its contents byte-by-byte to LPT1.
 * If no LPT1 hardware is present, reports an error.
 */

#include "cmd_internal.h"
#include "../dos/print_handler.h"

void Cmd_Print(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !args[0]) {
        PRINT("Usage: print <file>");
        return;
    }

    /* Check if LPT1 is present */
    if (!PrintHandler_IsPresent()) {
        PRINT("print: no parallel port (LPT1) detected");
        return;
    }

    /* Build absolute path */
    char path[CMD_MAX_PATH];
    const char *cwd = ctx->cwd ? ctx->cwd : "RAM:";
    cmd_make_abs(cwd, args, path, CMD_MAX_PATH);

    /* Open the file */
    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ)) {
        PRINT("print: cannot open file");
        return;
    }

    /* Send file contents to LPT1 */
    uint8_t buf[512];
    uint32_t total = 0;
    for (;;) {
        uint32_t n = VFS_Read(&fh, buf, sizeof(buf));
        if (n == 0) break;
        for (uint32_t i = 0; i < n; i++)
            PrintHandler_SendByte(buf[i]);
        total += n;
    }

    VFS_Close(&fh);

    /* Report */
    char msg[CMD_MAX_LINE];
    cmd_scopy(msg, "print: sent ", CMD_MAX_LINE);
    cmd_uint_to_dec(total, msg + cmd_slen(msg), CMD_MAX_LINE - cmd_slen(msg));
    cmd_scat(msg, " bytes to PRT:", CMD_MAX_LINE);
    PRINT(msg);
}
