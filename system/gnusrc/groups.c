/* groups.c — GNU coreutils 'groups' for UAOS gnu: layer
 *
 * Print group memberships.
 *   groups [OPTION]... [USER]...
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
    int nops = uaos_operands_count(argc);
    if (nops == 0) {
        put_line("root");
    } else {
        for (int i = 0; i < nops; i++) {
            const char *user = uaos_operand(argc, argv, i);
            if (user) { put_s(user); put_s(" : root"); put_c('\n'); }
        }
    }
    return 0;
}
