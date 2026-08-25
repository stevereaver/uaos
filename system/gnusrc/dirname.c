/* dirname.c — GNU coreutils 'dirname' for UAOS gnu: layer
 *
 * Strip last component from file name.
 *   dirname [OPTION] NAME...
 * Options: -z, --zero
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_zero = 0;

static void do_dirname(const char *name)
{
    /* strip trailing slashes */
    int len = (int)uaos_strlen(name);
    while (len > 0 && name[len - 1] == '/') len--;
    /* find last / or : */
    int last = -1;
    for (int i = 0; i < len; i++) {
        if (name[i] == '/' || name[i] == ':') last = i;
    }
    if (last < 0) {
        put_s(".");
    } else if (last == 0) {
        put_c('/');
    } else {
        /* if it's a colon (volume prefix), include it */
        if (name[last] == ':') {
            uaos_write(1, name, last + 1);
        } else {
            /* strip trailing slashes again */
            int end = last;
            while (end > 0 && name[end - 1] == '/') end--;
            uaos_write(1, name, end);
        }
    }
    if (opt_zero) put_c('\0'); else put_c('\n');
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"zero", 'z', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "z", long_opts, &li)) != -1) {
        switch (opt) {
            case 'z': opt_zero = 1; break;
            default:  return 1;
        }
    }
    int nops = uaos_operands_count(argc);
    if (nops == 0) { put_line("dirname: missing operand"); return 1; }
    for (int i = 0; i < nops; i++) {
        const char *name = uaos_operand(argc, argv, i);
        if (name) do_dirname(name);
    }
    return 0;
}
