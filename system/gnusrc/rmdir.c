/* rmdir.c — GNU coreutils 'rmdir' for UAOS gnu: layer
 *
 * Remove empty directories.
 *   rmdir [OPTION]... DIRECTORY...
 * Options: -p, --parents, -v, --verbose, --ignore-fail-on-non-empty
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_parents = 0;
static int opt_verbose = 0;
static int opt_ignore_nonempty = 0;

static int do_rmdir(const char *dir)
{
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(dir, path, sizeof(path));

    /* check if directory is empty */
    long dd = uaos_opendir(path);
    if (dd < 0) {
        put_s("rmdir: failed to remove '"); put_s(dir);
        put_line("': No such file or directory");
        return 1;
    }
    int has_entries = 0;
    struct uaos_dirent ent;
    while (uaos_readdir((int)dd, &ent) > 0) {
        if (ent.name[0] == '.' && (ent.name[1] == '\0' ||
            (ent.name[1] == '.' && ent.name[2] == '\0'))) continue;
        has_entries = 1;
        break;
    }
    uaos_closedir((int)dd);

    if (has_entries) {
        if (!opt_ignore_nonempty) {
            put_s("rmdir: failed to remove '"); put_s(dir);
            put_line("': Directory not empty");
        }
        return opt_ignore_nonempty ? 0 : 1;
    }

    if (uaos_delete(path) != 0) {
        put_s("rmdir: failed to remove '"); put_s(dir); put_line("'");
        return 1;
    }
    if (opt_verbose) {
        put_s("rmdir: removing directory, '"); put_s(dir); put_line("'");
    }

    if (opt_parents) {
        /* remove parent directories if empty */
        int len = (int)uaos_strlen(path);
        while (len > 0) {
            /* strip last component */
            int i = len - 1;
            while (i > 0 && path[i] != '/' && path[i] != ':') i--;
            if (i <= 0) break;
            path[i] = '\0';
            len = i;
            /* skip trailing colons */
            if (path[len - 1] == ':') break;
            dd = uaos_opendir(path);
            if (dd < 0) break;
            has_entries = 0;
            while (uaos_readdir((int)dd, &ent) > 0) {
                if (ent.name[0] == '.' && (ent.name[1] == '\0' ||
                    (ent.name[1] == '.' && ent.name[2] == '\0'))) continue;
                has_entries = 1; break;
            }
            uaos_closedir((int)dd);
            if (has_entries) break;
            uaos_delete(path);
            if (opt_verbose) {
                put_s("rmdir: removing directory, '"); put_s(path); put_line("'");
            }
        }
    }
    return 0;
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"parents",  'p', no_argument},
        {"verbose",  'v', no_argument},
        {"ignore-fail-on-non-empty", 0, no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "pv", long_opts, &li)) != -1) {
        switch (opt) {
            case 'p': opt_parents = 1; break;
            case 'v': opt_verbose = 1; break;
            case UAOS_GO_LONG + 0: opt_ignore_nonempty = 1; break;
            default:  return 1;
        }
    }
    int nops = uaos_operands_count(argc);
    if (nops == 0) { put_line("rmdir: missing operand"); return 1; }
    int rc = 0;
    for (int i = 0; i < nops; i++) {
        const char *dir = uaos_operand(argc, argv, i);
        if (dir) { if (do_rmdir(dir) != 0) rc = 1; }
    }
    return rc;
}
