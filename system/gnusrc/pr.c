/* pr.c — GNU coreutils 'pr' for UAOS gnu: layer
 *
 * Paginate or columnate files for printing.
 *   pr [OPTION]... [FILE]...
 * Options: -n N, --pages=N, -l N, --length=N, -w N, --width=N,
 *          -t, --omit-header, -f, -F, --form-feed, -o N, --indent=N,
 *          -N, --number-lines, -c, --show-control-chars
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int  opt_length = 66;
static int  opt_width = 72;
static int  opt_omit_header = 0;
static int  opt_number_lines = 0;
static int  opt_indent = 0;
static int  opt_columns = 1;

static void print_header(const char *fname, int page)
{
    if (opt_omit_header) return;
    /* Header: filename + date + page */
    put_s(fname);
    put_s("  Page ");
    char buf[12];
    int_to_dec(page, buf, sizeof(buf));
    put_s(buf);
    /* fill to width with spaces */
    int slen = (int)uaos_strlen(fname) + 8 + (int)uaos_strlen(buf);
    while (slen < opt_width) { put_c(' '); slen++; }
    put_c('\n');
    for (int i = 0; i < opt_width; i++) put_c('-');
    put_c('\n');
}

static void print_footer(void)
{
    if (opt_omit_header) return;
    put_c('\n');
    for (int i = 0; i < opt_width; i++) put_c('-');
    put_c('\n');
    put_c('\n');
}

static void pr_fd(int fd, uint32_t sz, int is_stdin, const char *fname)
{
    int page = 1;
    int line_in_page = 0;
    int line_no = 0;
    int header_lines = opt_omit_header ? 0 : 5; /* header + blank lines */
    int footer_lines = opt_omit_header ? 0 : 5;
    int content_lines = opt_length - header_lines - footer_lines;
    if (content_lines < 1) content_lines = 1;

    print_header(fname, page);

    char line[UAOS_CMD_LINE_MAX * 2];
    int col = 0;
    uint32_t pos = 0;
    for (;;) {
        uint8_t ch; long n;
        if (is_stdin) { n = uaos_read(fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
        if (n <= 0) break;

        if (ch == '\n') {
            line[col] = '\0';
            line_no++;
            /* indent */
            for (int i = 0; i < opt_indent; i++) put_c(' ');
            /* line number */
            if (opt_number_lines) {
                char num[8];
                int_to_dec(line_no, num, sizeof(num));
                int slen = (int)uaos_strlen(num);
                for (int i = slen; i < 5; i++) put_c(' ');
                put_s(num);
                put_c(' ');
            }
            put_s(line);
            put_c('\n');
            col = 0;
            line_in_page++;
            if (line_in_page >= content_lines) {
                print_footer();
                page++;
                print_header(fname, page);
                line_in_page = 0;
            }
        } else if (ch != '\r' && col < (int)sizeof(line) - 1) {
            line[col++] = (char)ch;
        }
    }
    if (col > 0) {
        for (int i = 0; i < opt_indent; i++) put_c(' ');
        put_s(line);
        put_c('\n');
    }
    print_footer();
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"length",     'l', required_argument},
        {"width",      'w', required_argument},
        {"omit-header",'t', no_argument},
        {"number-lines",'N', no_argument},
        {"indent",     'o', required_argument},
        {"columns",    0,   required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "l:w:tNo:", long_opts, &li)) != -1) {
        switch (opt) {
            case 'l': { long v; if (uaos_optarg_long(&v) && v > 0) opt_length = (int)v; } break;
            case 'w': { long v; if (uaos_optarg_long(&v) && v > 0) opt_width = (int)v; } break;
            case 't': opt_omit_header = 1; break;
            case 'N': opt_number_lines = 1; break;
            case 'o': { long v; if (uaos_optarg_long(&v)) opt_indent = (int)v; } break;
            default:  break;
        }
    }
    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) { pr_fd(0, 0, 1, ""); return 0; }
    for (int i = 0; i < nfiles; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (!fname) continue;
        if (fname[0] == '-' && fname[1] == '\0') { pr_fd(0, 0, 1, "-"); continue; }
        char path[UAOS_CMD_PATH_MAX]; cmd_make_abs(fname, path, sizeof(path));
        long fd = uaos_open(path, UAOS_O_RDONLY);
        if (fd < 0) { put_s("pr: "); put_s(fname); put_line(": No such file"); continue; }
        struct uaos_stat st; uint32_t sz = 0;
        if (uaos_stat(path, &st) == 0) sz = st.size;
        pr_fd((int)fd, sz, 0, fname);
        uaos_close((int)fd);
    }
    return 0;
}
