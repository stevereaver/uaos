/* mkdir.c — GNU coreutils 'mkdir' for UAOS gnu: layer
 *
 * Create directories.
 *   mkdir [OPTION]... DIRECTORY...
 * Options: -p, --parents, -m MODE, --mode=MODE, -v, --verbose
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_parents = 0;
static int opt_verbose = 0;
static uint16_t opt_mode = 0; /* AmigaDOS protection bits */

static int do_mkdir(const char *dir)
{
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(dir, path, sizeof(path));

    if (opt_parents) {
        /* create intermediate directories */
        int len = (int)uaos_strlen(path);
        for (int i = 1; i < len; i++) {
            if (path[i] == '/' || path[i] == ':') {
                char save = path[i];
                path[i] = '\0';
                struct uaos_stat st;
                if (uaos_stat(path, &st) != 0) {
                    uaos_mkdir(path);
                    if (opt_verbose) {
                        put_s("mkdir: created directory '");
                        put_s(path); put_line("'");
                    }
                }
                path[i] = save;
            }
        }
    }

    struct uaos_stat st;
    if (uaos_stat(path, &st) == 0) {
        if (!opt_parents) {
            put_s("mkdir: cannot create directory '"); put_s(dir);
            put_line("': File exists");
            return 1;
        }
        return 0;
    }

    if (uaos_mkdir(path) != 0) {
        put_s("mkdir: cannot create directory '"); put_s(dir); put_line("'");
        return 1;
    }

    if (opt_mode != 0) {
        uaos_setprotection(path, opt_mode);
    }

    if (opt_verbose) {
        put_s("mkdir: created directory '"); put_s(dir); put_line("'");
    }
    return 0;
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"parents", 'p', no_argument},
        {"mode",    'm', required_argument},
        {"verbose", 'v', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "pm:v", long_opts, &li)) != -1) {
        switch (opt) {
            case 'p': opt_parents = 1; break;
            case 'm':
                /* parse mode — simplified: just store raw value */
                if (g_optarg) {
                    long v = 0;
                    const char *p = g_optarg;
                    while (*p >= '0' && *p <= '7') { v = v * 8 + (*p - '0'); p++; }
                    opt_mode = (uint16_t)v;
                }
                break;
            case 'v': opt_verbose = 1; break;
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    if (nops == 0) { put_line("mkdir: missing operand"); return 1; }
    int rc = 0;
    for (int i = 0; i < nops; i++) {
        const char *dir = uaos_operand(argc, argv, i);
        if (dir) { if (do_mkdir(dir) != 0) rc = 1; }
    }
    return rc;
}
