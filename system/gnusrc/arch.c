/* arch.c — GNU coreutils 'arch' for UAOS gnu: layer
 *
 * Print machine hardware name.
 *   arch [OPTION]...
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
    put_line("x86_64");
    return 0;
}
