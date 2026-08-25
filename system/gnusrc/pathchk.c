/* pathchk.c — GNU coreutils 'pathchk' for UAOS gnu: layer
 *
 * Check whether file names are valid or portable.
 *   pathchk [OPTION]... NAME...
 * Options: -p, --portability, -P, --posix, --portability-check
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_portable = 0;
static int opt_posix = 0;

static int check_name(const char *name)
{
    int len = (int)uaos_strlen(name);
    if (len == 0) { put_s("pathchk: empty file name\n"); return 1; }
    if (len > 255) { put_s("pathchk: "); put_s(name); put_line(": too long"); return 1; }

    if (opt_posix || opt_portable) {
        /* POSIX portable filename: A-Za-z0-9._- only */
        for (int i = 0; i < len; i++) {
            char c = name[i];
            if (c == '/' || c == ':') continue; /* path separators */
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
                put_s("pathchk: "); put_s(name);
                put_line(": nonportable character");
                return 1;
            }
        }
        /* leading hyphen not portable */
        if (name[0] == '-') {
            put_s("pathchk: "); put_s(name);
            put_line(": leading hyphen");
            return 1;
        }
    }

    if (opt_portable) {
        /* portable: max 14 chars per component */
        int comp_len = 0;
        for (int i = 0; i <= len; i++) {
            if (name[i] == '\0' || name[i] == '/' || name[i] == ':') {
                if (comp_len > 14) {
                    put_s("pathchk: "); put_s(name);
                    put_line(": component too long for portability");
                    return 1;
                }
                comp_len = 0;
            } else {
                comp_len++;
            }
        }
    }
    return 0;
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"portability", 'p', no_argument},
        {"posix",       'P', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "pP", long_opts, &li)) != -1) {
        switch (opt) {
            case 'p': opt_portable = 1; break;
            case 'P': opt_posix = 1; break;
            default:  return 1;
        }
    }
    int nops = uaos_operands_count(argc);
    if (nops == 0) { put_line("pathchk: missing operand"); return 1; }
    int rc = 0;
    for (int i = 0; i < nops; i++) {
        const char *name = uaos_operand(argc, argv, i);
        if (name) { if (check_name(name) != 0) rc = 1; }
    }
    return rc;
}
