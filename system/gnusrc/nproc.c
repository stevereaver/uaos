/* nproc.c — GNU coreutils 'nproc' for UAOS gnu: layer
 *
 * Print the number of processing units available.
 *   nproc [OPTION]...
 * Options: --all, --ignore=N
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"all",    0, no_argument},
        {"ignore", 0, required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    long ignore = 0;
    while ((opt = uaos_getopt_long(argc, argv, "", long_opts, &li)) != -1) {
        switch (opt) {
            case UAOS_GO_LONG + 0: break; /* --all */
            case UAOS_GO_LONG + 1: { long v; if (uaos_optarg_long(&v)) ignore = v; } break;
            default:  return 1;
        }
    }
    long nproc = 1 - ignore;
    if (nproc < 1) nproc = 1;
    char buf[8];
    int_to_dec((int32_t)nproc, buf, sizeof(buf));
    put_line(buf);
    return 0;
}
