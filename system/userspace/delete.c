/* delete.c — UAOS x86-64 userspace 'delete' command
 *
 * AmigaDOS C:Delete — delete a file or directory.
 *   delete <path> [ALL] [QUIET] [FORCE]
 */

#include "uaos_cmd.h"
#include "uaos_template.h"

/* Recursive delete of a directory tree.  If pat is non-empty, only delete
 * entries whose names match pat (and do not remove the directory itself).
 * Returns 0 on success. */
static long delete_recursive(const char *path, int force, const char *pat)
{
    /* Try as a directory first. */
    long dd = uaos_opendir(path);
    if (dd >= 0) {
        struct uaos_dirent ent;
        while (uaos_readdir((int)dd, &ent) > 0) {
            if (ent.name[0] == '\0') continue;
            if (pat && pat[0] && !cmd_pattern_match(ent.name, pat)) continue;
            char sub[UAOS_CMD_PATH_MAX];
            cmd_join_path(path, ent.name, sub, sizeof(sub));
            delete_recursive(sub, force, pat);
        }
        uaos_closedir((int)dd);
        if (!pat || !pat[0]) return uaos_delete(path);
        return 0;
    }

    /* File (possibly filtered by pattern). */
    if (pat && pat[0]) {
        const char *name = path;
        const char *tmp = path;
        while (*tmp) { if (*tmp == ':' || *tmp == '/') name = tmp + 1; tmp++; }
        if (!cmd_pattern_match(name, pat)) return -1;
    }
    long rc = uaos_delete(path);
    if (rc == -4 && force) {
        long p = uaos_getprotection(path);
        uaos_setprotection(path, (uint16_t)p & ~UAOS_FIBF_DELETE);
        rc = uaos_delete(path);
    }
    return rc;
}

int main(int argc, const char **argv)
{
    char args[UAOS_TMPL_MAX_VAL];
    cmd_build_args(argc, argv, args, sizeof(args));

    UaosTmpl t;
    uaos_tmpl_run("FILE/M/A,ALL/S,QUIET/S,FORCE/S", &t, args);
    if (t.error[0]) { put_s("delete: "); put_line(t.error); return 20; }

    int all   = uaos_tmpl_switch(&t, "ALL");
    int quiet = uaos_tmpl_switch(&t, "QUIET");
    int force = uaos_tmpl_switch(&t, "FORCE");

    int n = uaos_tmpl_count(&t, "FILE");
    if (n == 0) {
        put_line("Usage: delete <path> [ALL] [QUIET] [FORCE]");
        return 5;
    }

    int rc = 0;
    for (int i = 0; i < n; i++) {
        const char *file = uaos_tmpl_multi(&t, "FILE", i);
        if (!file) continue;

        char path[UAOS_CMD_PATH_MAX], pat[UAOS_CMD_PATH_MAX];
        cmd_split_path_pat(file, path, pat);

        long rci;
        if (all || pat[0]) {
            rci = delete_recursive(path, force, pat);
        } else {
            rci = uaos_delete(path);
            if (rci == -4 && force) {
                long p = uaos_getprotection(path);
                uaos_setprotection(path, (uint16_t)p & ~UAOS_FIBF_DELETE);
                rci = uaos_delete(path);
            }
        }

        if (!quiet) {
            if (rci == 0) {
                put_s("Deleted: ");
                put_s(path);
                if (pat[0]) { put_s(" (pattern: "); put_s(pat); put_s(")"); }
                put_c('\n');
            } else if (rci == -2) {
                put_line("Directory not empty.");
            } else if (rci == -1) {
                put_line("Not found.");
            } else {
                put_s("Failed to delete: ");
                put_line(path);
            }
        }
        if (rci != 0) rc = 5;
    }
    return rc;
}
