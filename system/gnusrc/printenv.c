/* printenv.c — GNU coreutils 'printenv' for UAOS gnu: layer
 *
 * Print all or part of environment.
 *   printenv [VARIABLE]...
 * Options: -0, --null
 *
 * Note: UAOS does not yet have a full environment variable system.
 * This prints nothing (or returns failure for requested variables).
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_null = 0;

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"null", '0', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "0", long_opts, &li)) != -1) {
        switch (opt) {
            case '0': opt_null = 1; break;
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    if (nops == 0) {
        /* print all environment — empty for now */
        return 0;
    }

    /* print requested variables — not found, return 1 */
    int rc = 0;
    for (int i = 0; i < nops; i++) {
        const char *name = uaos_operand(argc, argv, i);
        if (name) {
            /* not found */
            rc = 1;
        }
    }
    return rc;
}
