/* cmd_resident.c — C:resident — manage resident commands
 *
 * Syntax:
 *   resident              - list all resident commands
 *   resident <cmd>        - make <cmd> resident (flushable)
 *   resident <cmd> pure   - make <cmd> permanently resident
 *   resident <cmd> remove - remove <cmd> from resident list
 *   resident flush        - remove all non-pure (flushable) residents
 *
 * Resident commands stay in memory, avoiding repeated disk access.
 * This is useful for frequently-used commands.
 */

#include "cmd_internal.h"
#include "resident_cmd.h"

/* Helper to print via ctx */
static void print_resident(void *ctx_ptr, const char *line)
{
    NativeCmdCtx *ctx = (NativeCmdCtx *)ctx_ptr;
    PRINT(line);
}

void Cmd_Resident(NativeCmdCtx *ctx, const char *args)
{
    /* No arguments - list all resident commands */
    if (!args || !*args) {
        PRINT("Resident commands:");
        PRINT("Name                 Type      Size");
        PRINT("-----------------------------------------");

        ResidentEntry *reg = Resident_GetRegistry();
        int found = 0;

        for (int i = 0; i < MAX_RESIDENT_CMDS; i++) {
            if (!reg[i].in_use) continue;

            found = 1;
            char buf[96];
            char *p = buf;

            /* Name (left-aligned, 20 chars) */
            int ni = 0;
            while (reg[i].name[ni] && ni < 19) {
                *p++ = reg[i].name[ni++];
            }
            while (ni < 20) { *p++ = ' '; ni++; }

            /* Type (8 chars) */
            const char *type_str = "UNKNOWN";
            switch (reg[i].type) {
                case RESIDENT_TYPE_NATIVE: type_str = "NATIVE"; break;
                case RESIDENT_TYPE_M68K:   type_str = "M68K  "; break;
                case RESIDENT_TYPE_SCRIPT: type_str = "SCRIPT"; break;
            }
            while (*type_str) *p++ = *type_str++;
            *p++ = ' ';
            *p++ = ' ';

            /* Size (8 chars, right-aligned) */
            uint32_t size = reg[i].size;
            char size_str[16];
            int si = 0;
            if (size == 0) {
                size_str[si++] = '0';
            } else {
                char tmp[16];
                int ti = 0;
                while (size > 0) {
                    tmp[ti++] = '0' + (size % 10);
                    size /= 10;
                }
                while (ti > 0) size_str[si++] = tmp[--ti];
            }
            size_str[si] = '\0';
            /* Pad to 8 chars */
            int pad = 8 - si;
            while (pad-- > 0) *p++ = ' ';
            for (int j = 0; j < si; j++) *p++ = size_str[j];

            /* Pure flag */
            if (reg[i].permanent) {
                *p++ = ' ';
                *p++ = '[';
                *p++ = 'P';
                *p++ = ']';
            }

            *p = '\0';
            PRINT(buf);
        }

        if (!found) {
            PRINT("(none)");
        }
        return;
    }

    /* Parse command name and subcommand */
    char cmd_name[MAX_RESIDENT_NAME];
    char subcmd[16];
    const char *p = args;

    /* Skip leading spaces */
    while (*p == ' ' || *p == '\t') p++;

    /* Extract command name */
    int i = 0;
    while (*p && *p != ' ' && *p != '\t' && i < MAX_RESIDENT_NAME - 1) {
        cmd_name[i++] = *p++;
    }
    cmd_name[i] = '\0';

    /* Skip spaces */
    while (*p == ' ' || *p == '\t') p++;

    /* Extract subcommand (if any) */
    subcmd[0] = '\0';
    if (*p) {
        i = 0;
        while (*p && *p != ' ' && *p != '\t' && i < 15) {
            subcmd[i++] = *p++;
        }
        subcmd[i] = '\0';
    }

    /* Convert subcmd to lowercase for comparison */
    for (int j = 0; subcmd[j]; j++) {
        if (subcmd[j] >= 'A' && subcmd[j] <= 'Z') subcmd[j] += 32;
    }

    /* Handle subcommands */
    if (subcmd[0] == '\0') {
        /* Just command name - make it resident (flushable) */
        /* First, find the command in the path */
        char path[128];
        int found = 0;

        /* Check if it's a native command (C:) */
        if (NativeCmd_Exists(cmd_name)) {
            /* It's a native command - we need to find its path */
            /* For native commands, we create a virtual path */
            cmd_scopy(path, "C:/", sizeof(path));
            cmd_scat(path, cmd_name, sizeof(path));
            found = 1;
        } else {
            /* Search PATH */
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
                        cmd_scopy(path, entry, sizeof(path));
                        if (path[ei-1] != ':' && path[ei-1] != '/')
                            cmd_scat(path, "/", sizeof(path));
                        cmd_scat(path, cmd_name, sizeof(path));
                        VfsFile test;
                        if (VFS_Open(&test, path, VFS_READ)) {
                            VFS_Close(&test);
                            found = 1;
                            break;
                        }
                    }
                }
            }

            /* Try current directory */
            if (!found) {
                cmd_scopy(path, ctx->cwd, sizeof(path));
                int cl = cmd_slen(path);
                if (cl > 0 && path[cl-1] != ':' && path[cl-1] != '/')
                    cmd_scat(path, "/", sizeof(path));
                cmd_scat(path, cmd_name, sizeof(path));
                VfsFile test;
                if (VFS_Open(&test, path, VFS_READ)) {
                    VFS_Close(&test);
                    found = 1;
                }
            }
        }

        if (!found) {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "Command not found: ", CMD_MAX_LINE);
            cmd_scat(msg, cmd_name, CMD_MAX_LINE);
            PRINT(msg);
            return;
        }

        /* Add to resident list (not permanent) */
        if (Resident_Add(cmd_name, path, 0) == 0) {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "Made resident: ", CMD_MAX_LINE);
            cmd_scat(msg, cmd_name, CMD_MAX_LINE);
            PRINT(msg);
        } else {
            PRINT("Failed to make resident (out of memory or slots?)");
        }
        return;
    }

    /* pure - make permanently resident */
    if (cmd_seq_ci(subcmd, "pure")) {
        /* Find the command in the path */
        char path[128];
        int found = 0;

        /* Check if it's a native command (C:) */
        if (NativeCmd_Exists(cmd_name)) {
            cmd_scopy(path, "C:/", sizeof(path));
            cmd_scat(path, cmd_name, sizeof(path));
            found = 1;
        } else {
            /* Search PATH */
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
                        cmd_scopy(path, entry, sizeof(path));
                        if (path[ei-1] != ':' && path[ei-1] != '/')
                            cmd_scat(path, "/", sizeof(path));
                        cmd_scat(path, cmd_name, sizeof(path));
                        VfsFile test;
                        if (VFS_Open(&test, path, VFS_READ)) {
                            VFS_Close(&test);
                            found = 1;
                            break;
                        }
                    }
                }
            }

            /* Try current directory */
            if (!found) {
                cmd_scopy(path, ctx->cwd, sizeof(path));
                int cl = cmd_slen(path);
                if (cl > 0 && path[cl-1] != ':' && path[cl-1] != '/')
                    cmd_scat(path, "/", sizeof(path));
                cmd_scat(path, cmd_name, sizeof(path));
                VfsFile test;
                if (VFS_Open(&test, path, VFS_READ)) {
                    VFS_Close(&test);
                    found = 1;
                }
            }
        }

        if (!found) {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "Command not found: ", CMD_MAX_LINE);
            cmd_scat(msg, cmd_name, CMD_MAX_LINE);
            PRINT(msg);
            return;
        }

        /* Add to resident list (permanent/pure) */
        if (Resident_Add(cmd_name, path, 1) == 0) {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "Made permanently resident: ", CMD_MAX_LINE);
            cmd_scat(msg, cmd_name, CMD_MAX_LINE);
            PRINT(msg);
        } else {
            PRINT("Failed to make resident (out of memory or slots?)");
        }
        return;
    }

    /* remove - remove from resident list */
    if (cmd_seq_ci(subcmd, "remove")) {
        if (Resident_Remove(cmd_name) == 0) {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "Removed from resident list: ", CMD_MAX_LINE);
            cmd_scat(msg, cmd_name, CMD_MAX_LINE);
            PRINT(msg);
        } else {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "Not resident: ", CMD_MAX_LINE);
            cmd_scat(msg, cmd_name, CMD_MAX_LINE);
            PRINT(msg);
        }
        return;
    }

    /* Handle special "flush" command */
    if (cmd_seq_ci(cmd_name, "flush")) {
        int removed = Resident_Flush();
        char msg[CMD_MAX_LINE];
        char num_str[12];
        cmd_scopy(msg, "Flushed ", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)removed, num_str, sizeof(num_str));
        cmd_scat(msg, num_str, CMD_MAX_LINE);
        cmd_scat(msg, " resident command(s)", CMD_MAX_LINE);
        PRINT(msg);
        return;
    }

    /* Unknown subcommand */
    PRINT("Usage: resident [<cmd> [pure|remove] | flush]");
}
