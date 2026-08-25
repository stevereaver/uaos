/* cat.c — GNU coreutils 'cat' for UAOS gnu: layer
 *
 * Concatenate files and print to stdout.
 *   cat [OPTION]... [FILE]...
 * Options: -n, --number, -b, --number-nonblank, -s, --squeeze-blank,
 *          -A, --show-all, -E, --show-ends, -T, --show-tabs,
 *          -v, --show-nonprinting, -e, -t
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_number = 0;
static int opt_nonblank = 0;
static int opt_squeeze = 0;
static int opt_ends = 0;
static int opt_tabs = 0;
static int opt_nonprint = 0;

static void cat_fd(int fd, uint32_t sz, int is_stdin)
{
    int line_no = 0;
    int at_line_start = 1;
    int prev_blank = 0;
    uint32_t pos = 0;

    for (;;) {
        uint8_t ch;
        long n;
        if (is_stdin) {
            n = uaos_read(fd, &ch, 1);
        } else {
            if (pos >= sz) break;
            n = uaos_read_file(fd, &ch, 1);
            pos++;
        }
        if (n <= 0) break;

        if (at_line_start) {
            int is_blank = (ch == '\n');
            if (opt_squeeze && is_blank && prev_blank) {
                /* skip this blank line */
                while (ch != '\n') {
                    if (is_stdin) { if (uaos_read(fd, &ch, 1) <= 0) break; }
                    else { if (pos >= sz) break; uaos_read_file(fd, &ch, 1); pos++; }
                }
                continue;
            }
            prev_blank = is_blank;

            if (opt_number && !opt_nonblank) {
                char num[12];
                int_to_dec(line_no + 1, num, sizeof(num));
                put_s(num);
                put_s("  ");
            } else if (opt_nonblank && !is_blank) {
                char num[12];
                int_to_dec(line_no + 1, num, sizeof(num));
                put_s(num);
                put_s("  ");
            }
            at_line_start = 0;
        }

        if (ch == '\n') {
            line_no++;
            if (opt_ends) put_c('$');
            put_c('\n');
            at_line_start = 1;
            continue;
        }

        if (ch == '\t') {
            if (opt_tabs) {
                put_s("^I");
            } else {
                put_c('\t');
            }
            continue;
        }

        if (opt_nonprint) {
            if (ch >= 32 && ch < 127) {
                put_c((char)ch);
            } else if (ch == 127) {
                put_s("^?");
            } else if (ch < 32) {
                put_c('^');
                put_c((char)(ch + 64));
            } else {
                put_c('M');
                put_c('-');
                if (ch < 128 + 32) {
                    put_c('^');
                    put_c((char)(ch - 128 + 64));
                } else if (ch == 127 + 128) {
                    put_s("^?");
                } else {
                    put_c((char)(ch - 128));
                }
            }
            continue;
        }

        put_c((char)ch);
    }
}

static void cat_file(const char *fname)
{
    if (fname[0] == '-' && fname[1] == '\0') {
        cat_fd(0, 0, 1);
        return;
    }
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(fname, path, sizeof(path));
    long fd = uaos_open(path, UAOS_O_RDONLY);
    if (fd < 0) {
        put_s("cat: ");
        put_s(fname);
        put_line(": No such file or directory");
        return;
    }
    struct uaos_stat st;
    uint32_t sz = 0;
    if (uaos_stat(path, &st) == 0) sz = st.size;
    cat_fd((int)fd, sz, 0);
    uaos_close((int)fd);
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"number",           'n', no_argument},
        {"number-nonblank",  'b', no_argument},
        {"squeeze-blank",    's', no_argument},
        {"show-all",         'A', no_argument},
        {"show-ends",        'E', no_argument},
        {"show-tabs",        'T', no_argument},
        {"show-nonprinting", 'v', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "nbAsAETvte", long_opts, &li)) != -1) {
        switch (opt) {
            case 'n': opt_number = 1; opt_nonblank = 0; break;
            case 'b': opt_nonblank = 1; opt_number = 0; break;
            case 's': opt_squeeze = 1; break;
            case 'A': opt_ends = 1; opt_tabs = 1; opt_nonprint = 1; break;
            case 'E': opt_ends = 1; break;
            case 'T': opt_tabs = 1; break;
            case 'v': opt_nonprint = 1; break;
            case 'e': opt_ends = 1; opt_nonprint = 1; break;
            case 't': opt_tabs = 1; opt_nonprint = 1; break;
            default:  return 1;
        }
    }

    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) {
        cat_fd(0, 0, 1);
        return 0;
    }
    for (int i = 0; i < nfiles; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (fname) cat_file(fname);
    }
    return 0;
}
