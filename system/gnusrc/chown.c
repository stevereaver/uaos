/* chown.c — GNU coreutils 'chown' for UAOS gnu: layer
 *
 * Change file owner and group.
 *   chown [OPTION]... [OWNER][:[GROUP]] FILE...
 * Options: -R, --recursive, -v, --verbose, -c, --changes, -f, --silent
 *
 * Note: UAOS is a single-user OS.  This tool accepts the arguments but
 * does not actually change ownership (all files are owned by root/admin).
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_recursive = 0;
static int opt_verbose = 0;

static int do_chown(const char *fname)
{
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(fname, path, sizeof(path));
    struct uaos_stat st;
    if (uaos_stat(path, &st) != 0) {
        put_s("chown: cannot access '"); put_s(fname); put_line("'");
        return 1;
    }

    if (st.is_dir && opt_recursive) {
        long dd = uaos_opendir(path);
        if (dd >= 0) {
            struct uaos_dirent ent;
            while (uaos_readdir((int)dd, &ent) > 0) {
                if (ent.name[0] == '.' && (ent.name[1] == '\0' ||
                    (ent.name[1] == '.' && ent.name[2] == '\0'))) continue;
                char child[UAOS_CMD_PATH_MAX];
                cmd_join_path(path, ent.name, child, sizeof(child));
                do_chown(child);
            }
            uaos_closedir((int)dd);
        }
    }

    if (opt_verbose) {
        put_s("changed ownership of '"); put_s(fname);
        put_line("' to root:root");
    }
    /* UAOS doesn't have a chown syscall — ownership is always root */
    return 0;
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"recursive", 'R', no_argument},
        {"verbose",   'v', no_argument},
        {"changes",   'c', no_argument},
        {"silent",    'f', no_argument},
        {"reference", 0,  required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "Rvcf", long_opts, &li)) != -1) {
        switch (opt) {
            case 'R': opt_recursive = 1; break;
            case 'v': opt_verbose = 1; break;
            case 'c': opt_verbose = 1; break;
            case 'f': break;
            case UAOS_GO_LONG + 0: break;
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    if (nops < 2) { put_line("chown: missing operand"); return 1; }

    /* skip the owner:group argument */
    int rc = 0;
    for (int i = 1; i < nops; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (fname) { if (do_chown(fname) != 0) rc = 1; }
    }
    return rc;
}
