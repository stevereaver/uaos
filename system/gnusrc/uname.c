/* uname.c — GNU coreutils 'uname' for UAOS gnu: layer
 *
 * Print system information.
 *   uname [OPTION]...
 * Options: -a, --all, -s, --kernel-name, -n, --nodename, -r, --kernel-release,
 *          -v, --kernel-version, -m, --machine, -p, --processor,
 *          -i, --hardware-platform, -o, --operating-system
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"all",              'a', no_argument},
        {"kernel-name",      's', no_argument},
        {"nodename",         'n', no_argument},
        {"kernel-release",   'r', no_argument},
        {"kernel-version",   'v', no_argument},
        {"machine",          'm', no_argument},
        {"processor",        'p', no_argument},
        {"hardware-platform",'i', no_argument},
        {"operating-system", 'o', no_argument},
        {NULL, 0, 0}
    };
    int opt_s = 0, opt_n = 0, opt_r = 0, opt_v = 0, opt_m = 0, opt_p = 0, opt_i = 0, opt_o = 0, opt_a = 0;
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "asnrvmpio", long_opts, &li)) != -1) {
        switch (opt) {
            case 'a': opt_a = 1; break;
            case 's': opt_s = 1; break;
            case 'n': opt_n = 1; break;
            case 'r': opt_r = 1; break;
            case 'v': opt_v = 1; break;
            case 'm': opt_m = 1; break;
            case 'p': opt_p = 1; break;
            case 'i': opt_i = 1; break;
            case 'o': opt_o = 1; break;
            default:  return 1;
        }
    }

    if (!opt_s && !opt_n && !opt_r && !opt_v && !opt_m && !opt_p && !opt_i && !opt_o)
        opt_s = 1; /* default */

    if (opt_a) { opt_s = opt_n = opt_r = opt_v = opt_m = opt_o = 1; }

    int first = 1;
    if (opt_s) { if (!first) put_c(' '); put_s("UAOS"); first = 0; }
    if (opt_n) { if (!first) put_c(' '); put_s("uaos"); first = 0; }
    if (opt_r) { if (!first) put_c(' '); put_s("1.0"); first = 0; }
    if (opt_v) { if (!first) put_c(' '); put_s("UAOS 1.0 (Amiga-inspired)"); first = 0; }
    if (opt_m) { if (!first) put_c(' '); put_s("x86_64"); first = 0; }
    if (opt_p) { if (!first) put_c(' '); put_s("x86_64"); first = 0; }
    if (opt_i) { if (!first) put_c(' '); put_s("x86_64"); first = 0; }
    if (opt_o) { if (!first) put_c(' '); put_s("GNU/Linux-compatible"); first = 0; }
    put_c('\n');
    return 0;
}
