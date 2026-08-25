/* basename.c — GNU coreutils 'basename' for UAOS gnu: layer
 *
 * Strip directory and suffix from filenames.
 *   basename NAME [SUFFIX]
 *   basename -a [OPTION]... NAME...  (multiple)
 *   basename -s SUFFIX NAME...
 * Options: -a, --multiple, -s SUFFIX, --suffix=SUFFIX, -z, --zero
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_multiple = 0;
static int opt_zero = 0;
static char opt_suffix[64] = "";

static void do_basename(const char *name, const char *suffix)
{
    /* strip trailing slashes */
    const char *end = name + uaos_strlen(name);
    while (end > name && end[-1] == '/') end--;
    /* find last / or : */
    const char *start = name;
    const char *p = name;
    while (p < end) {
        if (*p == '/' || *p == ':') start = p + 1;
        p++;
    }
    int len = (int)(end - start);
    /* strip suffix if specified and name ends with it */
    if (suffix && suffix[0]) {
        int sl = (int)uaos_strlen(suffix);
        if (len >= sl && uaos_strncmp(end - sl, suffix, sl) == 0) {
            len -= sl;
        }
    }
    if (len <= 0) { put_c('/'); }
    else { uaos_write(1, start, len); }
    if (opt_zero) put_c('\0'); else put_c('\n');
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"multiple", 'a', no_argument},
        {"suffix",   's', required_argument},
        {"zero",     'z', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "as:z", long_opts, &li)) != -1) {
        switch (opt) {
            case 'a': opt_multiple = 1; break;
            case 's': if (g_optarg) uaos_strcpy(opt_suffix, g_optarg); opt_multiple = 1; break;
            case 'z': opt_zero = 1; break;
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    if (nops == 0) { put_line("basename: missing operand"); return 1; }

    if (opt_multiple) {
        for (int i = 0; i < nops; i++) {
            const char *name = uaos_operand(argc, argv, i);
            if (name) do_basename(name, opt_suffix);
        }
    } else {
        /* traditional: basename NAME [SUFFIX] */
        const char *name = uaos_operand(argc, argv, 0);
        const char *suffix = (nops >= 2) ? uaos_operand(argc, argv, 1) : "";
        do_basename(name, suffix);
    }
    return 0;
}
