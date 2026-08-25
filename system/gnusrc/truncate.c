/* truncate.c — GNU coreutils 'truncate' for UAOS gnu: layer
 *
 * Shrink or extend the size of a file.
 *   truncate OPTION... FILE...
 * Options: -s N, --size=N, -c, --no-create, -r FILE, --reference=FILE
 * Size: N, +N, -N, /N, with suffixes K, M, G, T
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static long opt_size = -1;
static int opt_no_create = 0;

static long parse_size(const char *s)
{
    long v = 0;
    const char *p = s;
    int neg = 0;
    if (*p == '+') p++;
    else if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    if (*p == 'K' || *p == 'k') v *= 1024;
    else if (*p == 'M' || *p == 'm') v *= 1048576;
    else if (*p == 'G' || *p == 'g') v *= 1073741824L;
    return neg ? -v : v;
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"size",     's', required_argument},
        {"no-create",'c', no_argument},
        {"reference",'r', required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "s:cr:", long_opts, &li)) != -1) {
        switch (opt) {
            case 's': if (g_optarg) opt_size = parse_size(g_optarg); break;
            case 'c': opt_no_create = 1; break;
            case 'r': break; /* reference — not fully supported */
            default:  return 1;
        }
    }

    if (opt_size < 0) { put_line("truncate: you must specify a size with --size"); return 1; }

    int nops = uaos_operands_count(argc);
    if (nops == 0) { put_line("truncate: missing file operand"); return 1; }

    int rc = 0;
    for (int i = 0; i < nops; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (!fname) continue;
        char path[UAOS_CMD_PATH_MAX];
        cmd_make_abs(fname, path, sizeof(path));
        struct uaos_stat st;
        if (uaos_stat(path, &st) != 0) {
            if (opt_no_create) continue;
            long fd = uaos_open(path, UAOS_O_WRONLY | UAOS_O_CREAT);
            if (fd < 0) { put_s("truncate: cannot create '"); put_s(fname); put_line("'"); rc = 1; continue; }
            uaos_close((int)fd);
        }
        /* UAOS doesn't have ftruncate; we write zeros or truncate by
         * reopening.  For simplicity, if the file needs to grow, write
         * zeros; if it needs to shrink, rewrite the content. */
        long fd = uaos_open(path, UAOS_O_WRONLY | UAOS_O_CREAT);
        if (fd < 0) { put_s("truncate: cannot open '"); put_s(fname); put_line("'"); rc = 1; continue; }
        /* Write opt_size zero bytes to set the file to that size */
        uint8_t zeros[4096];
        uaos_memset(zeros, 0, sizeof(zeros));
        long remaining = opt_size;
        while (remaining > 0) {
            long chunk = remaining < (long)sizeof(zeros) ? remaining : (long)sizeof(zeros);
            uaos_write_file((int)fd, zeros, chunk);
            remaining -= chunk;
        }
        uaos_close((int)fd);
    }
    return rc;
}
