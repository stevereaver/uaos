/* rm.c — GNU coreutils 'rm' for UAOS gnu: layer
 *
 * Remove files or directories.
 *   rm [OPTION]... [FILE]...
 * Options: -r, -R, --recursive, -f, --force, -i, --interactive, -v, --verbose, -d, --dir
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_recursive = 0;
static int opt_force = 0;
static int opt_verbose = 0;
static int opt_dir = 0;

static int rm_recursive(const char *path)
{
    char apath[UAOS_CMD_PATH_MAX];
    cmd_make_abs(path, apath, sizeof(apath));
    struct uaos_stat st;
    if (uaos_stat(apath, &st) != 0) {
        if (opt_force) return 0;
        put_s("rm: cannot remove '"); put_s(path); put_line("': No such file");
        return 1;
    }

    if (st.is_dir) {
        if (!opt_recursive && !opt_dir) {
            put_s("rm: cannot remove '"); put_s(path);
            put_line("': Is a directory");
            return 1;
        }
        if (opt_recursive) {
            long dd = uaos_opendir(apath);
            if (dd >= 0) {
                struct uaos_dirent ent;
                while (uaos_readdir((int)dd, &ent) > 0) {
                    if (ent.name[0] == '.' && (ent.name[1] == '\0' ||
                        (ent.name[1] == '.' && ent.name[2] == '\0'))) continue;
                    char child[UAOS_CMD_PATH_MAX];
                    cmd_join_path(apath, ent.name, child, sizeof(child));
                    rm_recursive(child);
                }
                uaos_closedir((int)dd);
            }
        }
    }

    if (uaos_delete(apath) != 0) {
        if (!opt_force) {
            put_s("rm: cannot remove '"); put_s(path); put_line("'");
        }
        return 1;
    }
    if (opt_verbose) {
        put_s("removed '"); put_s(path); put_line("'");
    }
    return 0;
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"recursive", 'r', no_argument},
        {"force",     'f', no_argument},
        {"interactive",'i',no_argument},
        {"verbose",   'v', no_argument},
        {"dir",       'd', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "rRfivd", long_opts, &li)) != -1) {
        switch (opt) {
            case 'r': case 'R': opt_recursive = 1; break;
            case 'f': opt_force = 1; break;
            case 'i': break;
            case 'v': opt_verbose = 1; break;
            case 'd': opt_dir = 1; break;
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    if (nops == 0) {
        if (!opt_force) put_line("rm: missing operand");
        return opt_force ? 0 : 1;
    }

    int rc = 0;
    for (int i = 0; i < nops; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (fname) { if (rm_recursive(fname) != 0) rc = 1; }
    }
    return rc;
}
