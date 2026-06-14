/* cmd_assign.c — C:assign — create or list AmigaDOS assigns */

#include "cmd_internal.h"

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* Case-insensitive compare of two words (max len bytes) */
static int kw_match(const char *s, const char *kw)
{
    while (*s && *kw) {
        char cs = *s, ck = *kw;
        if (cs >= 'A' && cs <= 'Z') cs += 32;
        if (ck >= 'A' && ck <= 'Z') ck += 32;
        if (cs != ck) return 0;
        s++; kw++;
    }
    return *kw == '\0';
}

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
        PRINT("Usage: assign <name>: <target> [ADD | DEFER]");
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
    if (kw_match(name, "to")) {
        ni = 0;
        while (*p && *p != ' ' && ni < 31) { name[ni++] = *p++; }
        name[ni] = '\0';
        while (*p == ' ') p++;
    }

    /* Extract target (stop before ADD / DEFER) */
    int ti = 0;
    while (*p && *p != ' ' && ti < 63) { target[ti++] = *p++; }
    target[ti] = '\0';

    while (*p == ' ') p++;

    /* Check for ADD / DEFER keywords */
    int add = 0;
    int defer = 0;
    if (kw_match(p, "add")) {
        add = 1;
        p += 3;
        while (*p == ' ') p++;
        if (kw_match(p, "defer")) defer = 1;
    } else if (kw_match(p, "defer")) {
        defer = 1;
    }

    if (!name[0] || !target[0]) {
        PRINT("Usage: assign <name>: <target> [ADD | DEFER]");
        PRINT("Example: assign C: Workbench:C");
        return;
    }

    if (VFS_AddAssign(name, target, add, defer) == 0) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, add ? "Added " : "Assigned ", CMD_MAX_LINE);
        cmd_scat(msg, name, CMD_MAX_LINE);
        cmd_scat(msg, " -> ", CMD_MAX_LINE);
        cmd_scat(msg, target, CMD_MAX_LINE);
        if (add) cmd_scat(msg, " (ADD)", CMD_MAX_LINE);
        if (defer) cmd_scat(msg, " (DEFER)", CMD_MAX_LINE);
        PRINT(msg);
    } else {
        PRINT("Failed to create assign.");
    }
}
