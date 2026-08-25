/* touch.c — GNU coreutils 'touch' for UAOS gnu: layer
 *
 * Change file timestamps (and create if missing).
 *   touch [OPTION]... FILE...
 * Options: -a, -m, -c, --no-create, -d STRING, --date=STRING, -r FILE, --reference=FILE
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_no_create = 0;
static int opt_atime = 0;
static int opt_mtime = 0;

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"no-create",  'c', no_argument},
        {"date",       'd', required_argument},
        {"reference",  'r', required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "acmd:r:", long_opts, &li)) != -1) {
        switch (opt) {
            case 'a': opt_atime = 1; break;
            case 'm': opt_mtime = 1; break;
            case 'c': opt_no_create = 1; break;
            case 'd': break; /* date string — not fully supported */
            case 'r': break; /* reference file — not fully supported */
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    if (nops == 0) { put_line("touch: missing file operand"); return 1; }

    int rc = 0;
    for (int i = 0; i < nops; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (!fname) continue;
        char path[UAOS_CMD_PATH_MAX];
        cmd_make_abs(fname, path, sizeof(path));
        struct uaos_stat st;
        if (uaos_stat(path, &st) != 0) {
            if (opt_no_create) continue;
            /* create empty file */
            long fd = uaos_open(path, UAOS_O_WRONLY | UAOS_O_CREAT);
            if (fd < 0) {
                put_s("touch: cannot touch '"); put_s(fname); put_line("'");
                rc = 1;
            } else {
                uaos_close((int)fd);
            }
        }
        /* UAOS doesn't have a separate utime syscall; setattrs can be used
         * but mtime is managed by the kernel on write.  We just ensure
         * the file exists. */
    }
    return rc;
}
