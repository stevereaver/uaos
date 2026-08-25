/* mktemp.c — GNU coreutils 'mktemp' for UAOS gnu: layer
 *
 * Create a temporary file or directory, safely.
 *   mktemp [OPTION]... [TEMPLATE]
 * Options: -d, --directory, -q, --quiet, -t, --tmpdir, -p DIR, --tmpdir=DIR
 * Template: must end in XXX... (at least 3 X's), replaced with random chars.
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_directory = 0;
static int opt_quiet = 0;
static int opt_tmpdir = 0;

static uint32_t mktemp_state = 0xDEADBEEF;
static char mktemp_rand_char(void)
{
    uint32_t x = mktemp_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    mktemp_state = x;
    return (char)('a' + (x % 26));
}

static int make_temp(char *template)
{
    int len = (int)uaos_strlen(template);
    /* find trailing X's */
    int xstart = len;
    while (xstart > 0 && template[xstart - 1] == 'X') xstart--;
    int xcount = len - xstart;
    if (xcount < 3) {
        if (!opt_quiet) put_line("mktemp: too few X's in template");
        return 0;
    }
    for (int attempt = 0; attempt < 100; attempt++) {
        for (int i = xstart; i < len; i++)
            template[i] = mktemp_rand_char();
        if (opt_directory) {
            char path[UAOS_CMD_PATH_MAX];
            cmd_make_abs(template, path, sizeof(path));
            if (uaos_mkdir(path) == 0) return 1;
        } else {
            char path[UAOS_CMD_PATH_MAX];
            cmd_make_abs(template, path, sizeof(path));
            long fd = uaos_open(path, UAOS_O_WRONLY | UAOS_O_CREAT);
            if (fd >= 0) { uaos_close((int)fd); return 1; }
        }
    }
    if (!opt_quiet) put_line("mktemp: failed to create temp file");
    return 0;
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"directory", 'd', no_argument},
        {"quiet",     'q', no_argument},
        {"tmpdir",    't', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "dqt", long_opts, &li)) != -1) {
        switch (opt) {
            case 'd': opt_directory = 1; break;
            case 'q': opt_quiet = 1; break;
            case 't': opt_tmpdir = 1; break;
            default:  return 1;
        }
    }

    char template[UAOS_CMD_PATH_MAX];
    int nops = uaos_operands_count(argc);
    if (nops == 0) {
        /* default template */
        if (opt_tmpdir) uaos_strcpy(template, "T:tmp.XXXXXXXXXX");
        else uaos_strcpy(template, "tmp.XXXXXXXXXX");
    } else {
        const char *t = uaos_operand(argc, argv, 0);
        if (!t) return 1;
        if (opt_tmpdir) {
            /* prepend T: if no volume prefix */
            const char *p = t;
            int has_vol = 0;
            while (*p) { if (*p == ':') { has_vol = 1; break; } p++; }
            if (has_vol) uaos_strcpy(template, t);
            else { uaos_strcpy(template, "T:"); uaos_strlcat(template, t, sizeof(template)); }
        } else {
            uaos_strcpy(template, t);
        }
    }

    if (make_temp(template)) {
        put_s(template);
        put_c('\n');
        return 0;
    }
    return 1;
}
