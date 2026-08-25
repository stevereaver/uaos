/* nl.c — GNU coreutils 'nl' for UAOS gnu: layer
 *
 * Number lines of files.
 *   nl [OPTION]... [FILE]...
 * Options: -b (body style a/t/n), -v N, -i N, -w N, -s STRING, -n (ln/rn/rz)
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int  nl_style = 1;   /* 0=none, 1=all, 2=nonempty */
static long nl_start = 1;
static long nl_incr  = 1;
static int  nl_width = 6;
static char nl_sep[8] = "\t";
static int  nl_format = 1;  /* 0=ln, 1=rn, 2=rz */

static void print_number(long num)
{
    char buf[24];
    int_to_dec(num, buf, sizeof(buf));
    int slen = (int)uaos_strlen(buf);
    if (slen < nl_width) {
        int pad = nl_width - slen;
        if (nl_format == 0) {
            /* left-justified: number then spaces */
            put_s(buf);
            for (int i = 0; i < pad; i++) put_c(' ');
        } else if (nl_format == 2) {
            /* zero-padded */
            for (int i = 0; i < pad; i++) put_c('0');
            put_s(buf);
        } else {
            /* right-justified */
            for (int i = 0; i < pad; i++) put_c(' ');
            put_s(buf);
        }
    } else {
        put_s(buf);
    }
    put_s(nl_sep);
}

static void nl_fd(int fd, uint32_t sz, int is_stdin)
{
    long line_no = nl_start;
    int at_line_start = 1;
    int line_is_blank = 1;
    uint32_t pos = 0;

    for (;;) {
        uint8_t ch;
        long n;
        if (is_stdin) { n = uaos_read(fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
        if (n <= 0) break;

        if (at_line_start) {
            line_is_blank = (ch == '\n');
            int do_number = 0;
            if (nl_style == 1) do_number = 1;
            else if (nl_style == 2 && !line_is_blank) do_number = 1;

            if (do_number) {
                print_number(line_no);
                line_no += nl_incr;
            } else {
                /* print blank number field */
                for (int i = 0; i < nl_width; i++) put_c(' ');
                put_s(nl_sep);
            }
            at_line_start = 0;
        }

        put_c((char)ch);
        if (ch == '\n') {
            at_line_start = 1;
        }
    }
}

static void nl_file(const char *fname)
{
    if (fname[0] == '-' && fname[1] == '\0') {
        nl_fd(0, 0, 1);
        return;
    }
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(fname, path, sizeof(path));
    long fd = uaos_open(path, UAOS_O_RDONLY);
    if (fd < 0) {
        put_s("nl: ");
        put_s(fname);
        put_line(": No such file or directory");
        return;
    }
    struct uaos_stat st;
    uint32_t sz = 0;
    if (uaos_stat(path, &st) == 0) sz = st.size;
    nl_fd((int)fd, sz, 0);
    uaos_close((int)fd);
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"body-numbering", 'b', required_argument},
        {"starting-number", 'v', required_argument},
        {"line-increment",  'i', required_argument},
        {"number-width",    'w', required_argument},
        {"number-separator",'s', required_argument},
        {"number-format",   'n', required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "b:v:i:w:s:n:", long_opts, &li)) != -1) {
        switch (opt) {
            case 'b':
                if (g_optarg) {
                    if (g_optarg[0] == 'a') nl_style = 1;
                    else if (g_optarg[0] == 't') nl_style = 2;
                    else if (g_optarg[0] == 'n') nl_style = 0;
                }
                break;
            case 'v': { long v; if (uaos_optarg_long(&v)) nl_start = v; } break;
            case 'i': { long v; if (uaos_optarg_long(&v)) nl_incr = v; } break;
            case 'w': { long v; if (uaos_optarg_long(&v) && v > 0 && v < 20) nl_width = (int)v; } break;
            case 's':
                if (g_optarg) {
                    int i = 0;
                    while (g_optarg[i] && i < (int)sizeof(nl_sep) - 1) {
                        nl_sep[i] = g_optarg[i]; i++;
                    }
                    nl_sep[i] = '\0';
                }
                break;
            case 'n':
                if (g_optarg) {
                    if (g_optarg[0] == 'l' && g_optarg[1] == 'n') nl_format = 0;
                    else if (g_optarg[0] == 'r' && g_optarg[1] == 'z') nl_format = 2;
                    else nl_format = 1;
                }
                break;
            default: return 1;
        }
    }

    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) {
        nl_fd(0, 0, 1);
        return 0;
    }
    for (int i = 0; i < nfiles; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (fname) nl_file(fname);
    }
    return 0;
}
