/* hostname.c — GNU coreutils 'hostname' for UAOS gnu: layer
 *
 * Show or set the system's host name.
 *   hostname [OPTION]... [NAME]
 * Options: -f, --fqdn, -s, --short, -i, --ip-address, -d, --domain
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"fqdn",       'f', no_argument},
        {"short",      's', no_argument},
        {"ip-address", 'i', no_argument},
        {"domain",     'd', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "fsid", long_opts, &li)) != -1) {
        switch (opt) { default: break; }
    }
    int nops = uaos_operands_count(argc);
    if (nops > 0) {
        /* setting hostname not supported */
        put_s("hostname: cannot set hostname to '");
        put_s(uaos_operand(argc, argv, 0));
        put_line("'");
        return 1;
    }
    put_line("uaos");
    return 0;
}
