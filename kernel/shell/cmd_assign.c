/* cmd_assign.c — C:assign — create or list AmigaDOS assigns */

#include "cmd_internal.h"

void Cmd_Assign(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        /* List current assigns */
        char buf[512];
        int n = VFS_ListAssigns(buf, sizeof(buf));
        if (n > 0) {
            PRINT("Current assigns:");
            const char *p = buf;
            char line[64];
            int li = 0;
            while (*p && li < (int)sizeof(line) - 1) {
                if (*p == '\n') {
                    line[li] = '\0';
                    if (li > 0) PRINT(line);
                    li = 0;
                } else {
                    line[li++] = *p;
                }
                p++;
            }
            if (li > 0) { line[li] = '\0'; PRINT(line); }
        } else {
            PRINT("No assigns defined.");
        }
        PRINT("");
        PRINT("Usage: assign <name>: <target>");
        PRINT("Example: assign C: Workbench:C");
        return;
    }

    /* Parse assign name and target */
    char name[32];
    char target[64];
    const char *p = args;

    while (*p == ' ') p++;

    /* Extract assign name (e.g. "C:") */
    int ni = 0;
    while (*p && *p != ' ' && ni < 31) { name[ni++] = *p++; }
    name[ni] = '\0';

    while (*p == ' ') p++;

    /* Optional "TO" keyword */
    if ((name[0] == 'T' || name[0] == 't') &&
        (name[1] == 'O' || name[1] == 'o') &&
        name[2] == '\0') {
        ni = 0;
        while (*p && *p != ' ' && ni < 31) { name[ni++] = *p++; }
        name[ni] = '\0';
        while (*p == ' ') p++;
    }

    int ti = 0;
    while (*p && ti < 63) { target[ti++] = *p++; }
    target[ti] = '\0';

    if (!name[0] || !target[0]) {
        PRINT("Usage: assign <name>: <target>");
        PRINT("Example: assign C: Workbench:C");
        return;
    }

    if (VFS_AddAssign(name, target) == 0) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "Assigned ", CMD_MAX_LINE);
        cmd_scat(msg, name, CMD_MAX_LINE);
        cmd_scat(msg, " -> ", CMD_MAX_LINE);
        cmd_scat(msg, target, CMD_MAX_LINE);
        PRINT(msg);
    } else {
        PRINT("Failed to create assign.");
    }
}
