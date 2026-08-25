/* who.c — GNU coreutils 'who' for UAOS gnu: layer
 *
 * Show who is logged on.
 *   who [OPTION]... [FILE]
 * Options: -a, --all, -b, --boot, -q, --count, -H, --heading
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"all",    'a', no_argument},
        {"boot",   'b', no_argument},
        {"count",  'q', no_argument},
        {"heading",'H', no_argument},
        {NULL, 0, 0}
    };
    int opt_heading = 0;
    int opt_count = 0;
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "abqH", long_opts, &li)) != -1) {
        switch (opt) {
            case 'a': opt_heading = 1; break;
            case 'b': break;
            case 'q': opt_count = 1; break;
            case 'H': opt_heading = 1; break;
            default:  return 1;
        }
    }

    if (opt_count) {
        put_line("1 root");
        return 0;
    }

    if (opt_heading) {
        put_line("NAME     LINE     TIME");
    }
    put_line("root     con0     (console)");
    return 0;
}
