/* cmd_mount.c — C:mount — mount a handler device
 *
 * Syntax:
 *   mount <device> from <handler>
 *   mount <device>
 *     (tries L:<device-name> automatically)
 *
 * Examples:
 *   mount AUX: from L:aux-handler
 *   mount PORT:
 */

#include "cmd_internal.h"
#include "dos/handler_loader.h"
#include "dos/dos_list.h"

void Cmd_Mount(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: mount <device> [from <handler>]");
        PRINT("       mount AUX: from L:aux-handler");
        PRINT("       mount PORT:");
        return;
    }

    char device[32] = {0};
    char handler[64] = {0};

    const char *p = args;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        /* "from" keyword */
        if ((p[0]=='F'||p[0]=='f') && (p[1]=='R'||p[1]=='r') &&
            (p[2]=='O'||p[2]=='o') && (p[3]=='M'||p[3]=='m') &&
            (p[4]==' ' || p[4]=='\0')) {
            p += 4;
            while (*p == ' ') p++;
            int i = 0;
            while (*p && *p != ' ' && i < 63) handler[i++] = *p++;
            handler[i] = '\0';
            continue;
        }

        /* Positional arguments */
        if (!device[0]) {
            int i = 0;
            while (*p && *p != ' ' && i < 31) device[i++] = *p++;
            device[i] = '\0';
        } else if (!handler[0]) {
            int i = 0;
            while (*p && *p != ' ' && i < 63) handler[i++] = *p++;
            handler[i] = '\0';
        } else {
            while (*p && *p != ' ') p++;
        }
    }

    if (!device[0]) {
        PRINT("mount: no device specified.");
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 20);
        return;
    }

    /* If handler not given, derive from device name:
     * AUX: -> aux-handler, PORT: -> port-handler */
    if (!handler[0]) {
        int dl = 0;
        while (device[dl] && device[dl] != ':' && dl < 31) {
            char c = device[dl];
            if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
            handler[dl] = c;
            dl++;
        }
        if (dl + 8 < 63) {
            handler[dl++] = '-';
            handler[dl++] = 'h';
            handler[dl++] = 'a';
            handler[dl++] = 'n';
            handler[dl++] = 'd';
            handler[dl++] = 'l';
            handler[dl++] = 'e';
            handler[dl++] = 'r';
            handler[dl] = '\0';
        }
    }

    /* Strip "L:" prefix from handler name if present */
    const char *hname = handler;
    if ((handler[0]=='L'||handler[0]=='l') && handler[1]==':') {
        hname = handler + 2;
        /* Also skip leading slash */
        if (*hname == '/') hname++;
    }

    /* Check if already mounted */
    LHandlerEntry *existing = HandlerLoader_FindByDevice(device);
    if (existing && existing->is_running) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "mount: ", CMD_MAX_LINE);
        cmd_scat(msg, device, CMD_MAX_LINE);
        cmd_scat(msg, " is already mounted.", CMD_MAX_LINE);
        PRINT(msg);
        return;
    }

    LHandlerEntry *entry = HandlerLoader_Load(hname);
    if (!entry || !entry->is_running) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "mount: failed to load handler '", CMD_MAX_LINE);
        cmd_scat(msg, hname, CMD_MAX_LINE);
        cmd_scat(msg, "' for ", CMD_MAX_LINE);
        cmd_scat(msg, device, CMD_MAX_LINE);
        PRINT(msg);
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 5);
        return;
    }

    /* Register in DosList */
    DosList_AddDevice(device, entry->port, ID_DOS_DISK);

    char msg[CMD_MAX_LINE];
    cmd_scopy(msg, "mount: ", CMD_MAX_LINE);
    cmd_scat(msg, device, CMD_MAX_LINE);
    cmd_scat(msg, " mounted via ", CMD_MAX_LINE);
    cmd_scat(msg, hname, CMD_MAX_LINE);
    PRINT(msg);
}
