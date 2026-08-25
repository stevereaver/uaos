/* id.c — GNU coreutils 'id' for UAOS gnu: layer
 *
 * Print user and group information.
 *   id [OPTION]... [USER]
 * Options: -u, --user, -g, --group, -G, --groups, -n, --name, -r, --real, -z, --zero
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_user = 0;
static int opt_group = 0;
static int opt_groups = 0;
static int opt_name = 0;

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"user",   'u', no_argument},
        {"group",  'g', no_argument},
        {"groups", 'G', no_argument},
        {"name",   'n', no_argument},
        {"real",   'r', no_argument},
        {"zero",   'z', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "ugGnrz", long_opts, &li)) != -1) {
        switch (opt) {
            case 'u': opt_user = 1; break;
            case 'g': opt_group = 1; break;
            case 'G': opt_groups = 1; break;
            case 'n': opt_name = 1; break;
            case 'r': break;
            case 'z': break;
            default:  return 1;
        }
    }

    if (opt_user) {
        if (opt_name) put_line("root"); else put_line("0");
        return 0;
    }
    if (opt_group) {
        if (opt_name) put_line("root"); else put_line("0");
        return 0;
    }
    if (opt_groups) {
        if (opt_name) put_line("root"); else put_line("0");
        return 0;
    }

    /* default: full id output */
    put_line("uid=0(root) gid=0(root) groups=0(root)");
    return 0;
}
