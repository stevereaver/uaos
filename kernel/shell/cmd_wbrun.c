/* cmd_wbrun.c — C:wbrun — launch a tool with Workbench semantics (UAOS-253)
 *
 * AmigaOS equivalent of launching via icon: reads the target's .info,
 * resolves project default tools, builds a WBStartup message and starts
 * the M68k binary with pr_CLI = 0.  Additional arguments become extra
 * WBArgs (like shift-clicked icons).
 *
 *   wbrun <tool-or-project> [extra-project ...]
 */

#include "cmd_internal.h"
#include "exec_file.h"

void Cmd_WBRun(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: wbrun <tool-or-project> [extra ...]");
        PRINT("Launches a program as if double-clicked on Workbench:");
        PRINT("WBStartup message, tooltypes, project default tool.");
        return;
    }

    /* Split args into whitespace-separated paths (no quoting support —
     * matches the rest of the command parser). */
    char paths[9][96];
    const char *argv[8];
    int n = 0;
    const char *p = args;
    while (*p && n < 9) {
        while (*p == ' ') p++;
        if (!*p) break;
        int i = 0;
        while (*p && *p != ' ' && i < 95) paths[n][i++] = *p++;
        paths[n][i] = '\0';
        n++;
    }
    if (!n) return;

    /* First arg = clicked icon; the rest are extra WBArgs. */
    for (int i = 1; i < n; i++) argv[i - 1] = paths[i];

    /* Resolve a bare name against the shell cwd if needed. */
    char full[128];
    const char *target = paths[0];
    int has_colon = 0;
    for (int i = 0; target[i]; i++) if (target[i] == ':') { has_colon = 1; break; }
    if (!has_colon && target[0] != '/') {
        int i = 0;
        const char *cwd = ctx->cwd ? ctx->cwd : "";
        while (cwd[i] && i < 120) { full[i] = cwd[i]; i++; }
        if (i && full[i - 1] != ':' && full[i - 1] != '/' && i < 120)
            full[i++] = '/';
        int j = 0;
        while (target[j] && i < 127) full[i++] = target[j++];
        full[i] = '\0';
        target = full;
    }

    int r = ExecFile_RunWB(target, argv, n - 1);
    if (r == -1)      PRINT("wbrun: object not found");
    else if (r == -2) PRINT("wbrun: not executable / bad format");
}
