/* fmt.c — GNU coreutils 'fmt' for UAOS gnu: layer
 *
 * Simple text formatter.
 *   fmt [OPTION]... [FILE]...
 * Options: -w N, --width=N, -s, --split-only, -c, --crown-margin,
 *          -t, --tagged-paragraph, -u, --uniform-spacing
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int  opt_width = 75;
static int  opt_split = 0;
static int  opt_crown = 0;
static int  opt_tagged = 0;
static int  opt_uniform = 0;

static void fmt_fd(int fd, uint32_t sz, int is_stdin)
{
    char line[UAOS_CMD_LINE_MAX * 2];
    int col = 0;
    int at_para_start = 1;
    int line_is_tag = 0;
    int prev_was_tag = 0;
    uint32_t pos = 0;

    /* word buffer */
    char word[256];
    int wlen = 0;
    int out_col = 0;
    int first_word = 1;

    for (;;) {
        uint8_t ch;
        long n;
        if (is_stdin) { n = uaos_read(fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
        if (n <= 0) break;

        if (ch == '\n') {
            line[col] = '\0';
            /* process accumulated word */
            if (wlen > 0) {
                if (!first_word && out_col + 1 + wlen > opt_width) {
                    put_c('\n');
                    out_col = 0;
                    first_word = 1;
                }
                if (!first_word) { put_c(' '); out_col++; }
                uaos_write(1, word, wlen);
                out_col += wlen;
                first_word = 0;
                wlen = 0;
            }
            /* check for paragraph break (empty line) */
            if (col == 0) {
                if (out_col > 0) { put_c('\n'); out_col = 0; first_word = 1; at_para_start = 1; }
                put_c('\n');
            } else {
                /* end of line — if not split-only, continue accumulating */
                if (opt_split) {
                    if (out_col > 0) { put_c('\n'); out_col = 0; first_word = 1; }
                }
                at_para_start = 0;
            }
            col = 0;
            continue;
        }

        if (ch != '\r') {
            if (col < (int)sizeof(line) - 1) line[col++] = (char)ch;
        }
        (void)line;
    }
    /* flush last word */
    if (wlen > 0) {
        if (!first_word && out_col + 1 + wlen > opt_width) {
            put_c('\n'); out_col = 0; first_word = 1;
        }
        if (!first_word) { put_c(' '); out_col++; }
        uaos_write(1, word, wlen);
    }
    if (out_col > 0) put_c('\n');
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"width",           'w', required_argument},
        {"split-only",      's', no_argument},
        {"crown-margin",    'c', no_argument},
        {"tagged-paragraph",'t', no_argument},
        {"uniform-spacing", 'u', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "w:sctu", long_opts, &li)) != -1) {
        switch (opt) {
            case 'w': { long v; if (uaos_optarg_long(&v) && v > 0) opt_width = (int)v; } break;
            case 's': opt_split = 1; break;
            case 'c': opt_crown = 1; break;
            case 't': opt_tagged = 1; break;
            case 'u': opt_uniform = 1; break;
            default:  return 1;
        }
    }

    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) { fmt_fd(0, 0, 1); return 0; }
    for (int i = 0; i < nfiles; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (!fname) continue;
        if (fname[0] == '-' && fname[1] == '\0') { fmt_fd(0, 0, 1); continue; }
        char path[UAOS_CMD_PATH_MAX];
        cmd_make_abs(fname, path, sizeof(path));
        long fd = uaos_open(path, UAOS_O_RDONLY);
        if (fd < 0) { put_s("fmt: "); put_s(fname); put_line(": No such file"); continue; }
        struct uaos_stat st; uint32_t sz = 0;
        if (uaos_stat(path, &st) == 0) sz = st.size;
        fmt_fd((int)fd, sz, 0);
        uaos_close((int)fd);
    }
    return 0;
}
