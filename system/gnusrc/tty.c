/* tty.c — GNU coreutils 'tty' for UAOS gnu: layer
 *
 * Print the file name of the terminal connected to standard input.
 *   tty [OPTION]...
 * Options: -s, --silent, --quiet
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_silent = 0;

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"silent", 's', no_argument},
        {"quiet",  'q', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "sq", long_opts, &li)) != -1) {
        switch (opt) {
            case 's': case 'q': opt_silent = 1; break;
            default:  return 1;
        }
    }
    if (!opt_silent) put_line("con0");
    return 0;
}
