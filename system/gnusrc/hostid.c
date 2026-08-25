/* hostid.c — GNU coreutils 'hostid' for UAOS gnu: layer
 *
 * Print the numeric identifier for the current host.
 *   hostid [OPTION]...
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = { {NULL, 0, 0} };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "", long_opts, &li)) != -1) {
        return 1;
    }
    /* fixed host ID for UAOS */
    put_line("0a000001");
    return 0;
}
