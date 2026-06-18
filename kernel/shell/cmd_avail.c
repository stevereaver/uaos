/* cmd_avail.c — C:avail — show available memory and handler status */

#include "cmd_internal.h"
#include "dos/handler_loader.h"

void Cmd_Avail(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    PRINT("Type       Total        Used        Free");
    PRINT("----------------------------------------");
    PRINT("RAM        512 MB       ~2 MB       ~510 MB");
    PRINT("Pool       1 MB         ~32 KB      ~992 KB");
    PRINT("Nodes      1024         variable    variable");
    PRINT("");
    PRINT("Kernel:    loaded at 0x00100000");
    PRINT("Stack:     16 KB per task (bootstrap)");
    PRINT("Framebuffer: mapped GOP region");
    PRINT("");

    /* Handler list */
    LHandlerEntry *entries[MAX_L_HANDLERS];
    int n = HandlerLoader_ListAll(entries, MAX_L_HANDLERS);
    if (n > 0) {
        PRINT("Handlers:");
        PRINT("Name                 Device        Type       Status");
        PRINT("--------------------------------------------------------");
        for (int i = 0; i < n; i++) {
            LHandlerEntry *e = entries[i];
            char line[CMD_MAX_LINE];
            cmd_scopy(line, e->name, CMD_MAX_LINE);
            int nl = cmd_slen(line);
            while (nl < 20) { line[nl++] = ' '; line[nl] = '\0'; }
            cmd_scat(line, e->device_name, CMD_MAX_LINE);
            nl = cmd_slen(line);
            while (nl < 34) { line[nl++] = ' '; line[nl] = '\0'; }
            if (e->is_filesystem) {
                cmd_scat(line, "filesystem ", CMD_MAX_LINE);
            } else {
                cmd_scat(line, "device     ", CMD_MAX_LINE);
            }
            cmd_scat(line, e->is_running ? "running" : "stopped", CMD_MAX_LINE);
            PRINT(line);
        }
    } else {
        PRINT("No handlers registered.");
    }
}
