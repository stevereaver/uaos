/* fold.c — GNU coreutils 'fold' for UAOS gnu: layer
 *
 * Wrap each input line to fit in specified width.
 *   fold [OPTION]... [FILE]...
 * Options: -b, --bytes, -s, --spaces, -w N, --width=N
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_width = 80;
static int opt_spaces = 0;
static int opt_bytes = 0;

static void fold_fd(int fd, uint32_t sz, int is_stdin)
{
    int col = 0;
    int last_space = -1;
    char buf[512];
    int bufpos = 0;
    uint32_t pos = 0;

    for (;;) {
        uint8_t ch;
        long n;
        if (is_stdin) { n = uaos_read(fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
        if (n <= 0) break;

        if (ch == '\n') {
            buf[bufpos] = '\0';
            put_s(buf);
            put_c('\n');
            bufpos = 0; col = 0; last_space = -1;
            continue;
        }

        if (bufpos >= (int)sizeof(buf) - 1) {
            buf[bufpos] = '\0';
            put_s(buf);
            bufpos = 0;
        }
        buf[bufpos++] = (char)ch;
        col++;

        if (opt_spaces && (ch == ' ' || ch == '\t')) last_space = bufpos - 1;

        if (col >= opt_width) {
            if (opt_spaces && last_space >= 0) {
                /* break at last space */
                buf[last_space] = '\0';
                put_s(buf);
                put_c('\n');
                /* move remainder to front */
                int rem = bufpos - last_space - 1;
                uaos_memcpy(buf, buf + last_space + 1, rem);
                bufpos = rem;
                col = rem;
                last_space = -1;
            } else {
                buf[bufpos] = '\0';
                put_s(buf);
                put_c('\n');
                bufpos = 0; col = 0; last_space = -1;
            }
        }
    }
    if (bufpos > 0) {
        buf[bufpos] = '\0';
        put_s(buf);
        put_c('\n');
    }
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"bytes",  'b', no_argument},
        {"spaces", 's', no_argument},
        {"width",  'w', required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "bsw:", long_opts, &li)) != -1) {
        switch (opt) {
            case 'b': opt_bytes = 1; break;
            case 's': opt_spaces = 1; break;
            case 'w': { long v; if (uaos_optarg_long(&v) && v > 0) opt_width = (int)v; } break;
            default:  return 1;
        }
    }

    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) { fold_fd(0, 0, 1); return 0; }
    for (int i = 0; i < nfiles; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (!fname) continue;
        if (fname[0] == '-' && fname[1] == '\0') { fold_fd(0, 0, 1); continue; }
        char path[UAOS_CMD_PATH_MAX];
        cmd_make_abs(fname, path, sizeof(path));
        long fd = uaos_open(path, UAOS_O_RDONLY);
        if (fd < 0) { put_s("fold: "); put_s(fname); put_line(": No such file"); continue; }
        struct uaos_stat st; uint32_t sz = 0;
        if (uaos_stat(path, &st) == 0) sz = st.size;
        fold_fd((int)fd, sz, 0);
        uaos_close((int)fd);
    }
    return 0;
}
