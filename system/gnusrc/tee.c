/* tee.c — GNU coreutils 'tee' for UAOS gnu: layer
 *
 * Read from stdin and write to stdout and files.
 *   tee [OPTION]... [FILE]...
 * Options: -a, --append, -i, --ignore-interrupts, -p, --output-error
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_append = 0;

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"append",  'a', no_argument},
        {"ignore-interrupts", 'i', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "aip", long_opts, &li)) != -1) {
        switch (opt) {
            case 'a': opt_append = 1; break;
            case 'i': case 'p': break;
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    long fds[16];
    int nfiles = 0;
    for (int i = 0; i < nops && i < 16; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (!fname) continue;
        char path[UAOS_CMD_PATH_MAX];
        cmd_make_abs(fname, path, sizeof(path));
        int flags = UAOS_O_WRONLY | UAOS_O_CREAT;
        if (opt_append) flags |= 0; /* no O_APPEND in UAOS; we'll seek end */
        else flags |= UAOS_O_TRUNC;
        long fd = uaos_open(path, flags);
        if (fd < 0) {
            put_s("tee: "); put_s(fname); put_line(": cannot open for writing");
            continue;
        }
        fds[nfiles++] = fd;
    }

    for (;;) {
        uint8_t buf[4096];
        long n = uaos_read(0, buf, sizeof(buf));
        if (n <= 0) break;
        uaos_write(1, buf, n);
        for (int i = 0; i < nfiles; i++) {
            uaos_write_file((int)fds[i], buf, n);
        }
    }

    for (int i = 0; i < nfiles; i++) uaos_close((int)fds[i]);
    return 0;
}
