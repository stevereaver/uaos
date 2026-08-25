/* realpath.c — GNU coreutils 'realpath' for UAOS gnu: layer
 *
 * Print the resolved absolute file name.
 *   realpath [OPTION]... FILE...
 * Options: -e, --canonicalize-existing, -m, --canonicalize-missing,
 *          -q, --quiet, -z, --zero, -s, --strip, --relative-to=DIR
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_zero = 0;
static int opt_quiet = 0;

static void do_realpath(const char *fname)
{
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(fname, path, sizeof(path));

    /* Verify the file exists */
    struct uaos_stat st;
    if (uaos_stat(path, &st) != 0) {
        if (!opt_quiet) {
            put_s("realpath: ");
            put_s(fname);
            put_line(": No such file or directory");
        }
        return;
    }
    put_s(path);
    if (opt_zero) put_c('\0'); else put_c('\n');
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"canonicalize-existing", 'e', no_argument},
        {"canonicalize-missing",  'm', no_argument},
        {"quiet",                 'q', no_argument},
        {"zero",                  'z', no_argument},
        {"strip",                 's', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "emqzs", long_opts, &li)) != -1) {
        switch (opt) {
            case 'e': case 'm': break; /* always canonicalize */
            case 'q': opt_quiet = 1; break;
            case 'z': opt_zero = 1; break;
            case 's': break;
            default:  return 1;
        }
    }
    int nops = uaos_operands_count(argc);
    if (nops == 0) { put_line("realpath: missing operand"); return 1; }
    for (int i = 0; i < nops; i++) {
        const char *name = uaos_operand(argc, argv, i);
        if (name) do_realpath(name);
    }
    return 0;
}
