/* cut.c — GNU coreutils 'cut' for UAOS gnu: layer
 *
 * Remove sections from each line of files.
 *   cut OPTION... [FILE]...
 * Options: -b LIST, -c LIST, -f LIST, -d CHAR, -s, --complement
 * LIST: "1-3,5,7-10" with open ranges "1-" and "-3".
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

#define CUT_MAX_RANGES 64

typedef struct { int lo; int hi; } Range;
static Range ranges[CUT_MAX_RANGES];
static int   range_count = 0;
static int   opt_complement = 0;
static char  opt_delim = '\t';
static int   opt_only_delim = 0;
static int   opt_mode = 0;  /* 1=bytes, 2=chars, 3=fields */

static void parse_list(const char *list)
{
    range_count = 0;
    const char *p = list;
    while (*p && range_count < CUT_MAX_RANGES) {
        int lo = 0, hi = 0;
        int have_lo = 0, have_hi = 0;
        if (*p >= '0' && *p <= '9') {
            while (*p >= '0' && *p <= '9') { lo = lo * 10 + (*p - '0'); p++; }
            have_lo = 1;
        }
        if (*p == '-') {
            p++;
            if (*p >= '0' && *p <= '9') {
                while (*p >= '0' && *p <= '9') { hi = hi * 10 + (*p - '0'); p++; }
                have_hi = 1;
            }
        }
        if (!have_lo && !have_hi) { if (*p) p++; continue; }
        if (!have_lo) lo = 1;
        if (!have_hi) hi = 0x7FFFFFFF;
        ranges[range_count].lo = lo;
        ranges[range_count].hi = hi;
        range_count++;
        while (*p == ',') p++;
    }
}

static int in_ranges(int pos)
{
    for (int i = 0; i < range_count; i++) {
        if (pos >= ranges[i].lo && pos <= ranges[i].hi)
            return 1;
    }
    return 0;
}

static void cut_line(const char *line, int len)
{
    int pos = 1;
    int field_idx = 1;
    int output_started = 0;

    if (opt_mode == 3) {
        /* fields mode */
        int has_delim = 0;
        for (int i = 0; i < len; i++) if (line[i] == opt_delim) { has_delim = 1; break; }
        if (!has_delim) {
            if (!opt_only_delim) { put_s(line); put_c('\n'); }
            return;
        }
        for (int i = 0; i <= len; i++) {
            if (i == len || line[i] == opt_delim) {
                int print = opt_complement ? !in_ranges(field_idx) : in_ranges(field_idx);
                if (print) {
                    if (output_started) put_c(opt_delim);
                    for (int j = pos - 1; j < i; j++) put_c(line[j]);
                    output_started = 1;
                }
                field_idx++;
                pos = i + 2;
            }
        }
        put_c('\n');
    } else {
        /* bytes/chars mode (identical for ASCII) */
        for (int i = 0; i < len; i++) {
            int print = opt_complement ? !in_ranges(pos) : in_ranges(pos);
            if (print) put_c(line[i]);
            pos++;
        }
        put_c('\n');
    }
}

static void cut_fd(int fd, uint32_t sz, int is_stdin)
{
    char line[UAOS_CMD_LINE_MAX * 4];
    int col = 0;
    uint32_t pos = 0;
    for (;;) {
        uint8_t ch;
        long n;
        if (is_stdin) { n = uaos_read(fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
        if (n <= 0) break;
        if (ch == '\n') {
            line[col] = '\0';
            cut_line(line, col);
            col = 0;
        } else if (ch != '\r') {
            if (col < (int)sizeof(line) - 1) line[col++] = (char)ch;
        }
    }
    if (col > 0) { line[col] = '\0'; cut_line(line, col); }
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"bytes",         'b', required_argument},
        {"characters",    'c', required_argument},
        {"fields",        'f', required_argument},
        {"delimiter",     'd', required_argument},
        {"only-delimited",'s', no_argument},
        {"complement",     1,  no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "b:c:f:d:s", long_opts, &li)) != -1) {
        switch (opt) {
            case 'b': opt_mode = 1; parse_list(g_optarg); break;
            case 'c': opt_mode = 2; parse_list(g_optarg); break;
            case 'f': opt_mode = 3; parse_list(g_optarg); break;
            case 'd': if (g_optarg) opt_delim = g_optarg[0]; break;
            case 's': opt_only_delim = 1; break;
            case UAOS_GO_LONG + 5: opt_complement = 1; break;
            default:  return 1;
        }
    }

    if (opt_mode == 0) {
        put_line("cut: you must specify a list of bytes, characters, or fields");
        return 1;
    }

    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) { cut_fd(0, 0, 1); return 0; }
    for (int i = 0; i < nfiles; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (!fname) continue;
        if (fname[0] == '-' && fname[1] == '\0') { cut_fd(0, 0, 1); continue; }
        char path[UAOS_CMD_PATH_MAX];
        cmd_make_abs(fname, path, sizeof(path));
        long fd = uaos_open(path, UAOS_O_RDONLY);
        if (fd < 0) { put_s("cut: "); put_s(fname); put_line(": No such file"); continue; }
        struct uaos_stat st; uint32_t sz = 0;
        if (uaos_stat(path, &st) == 0) sz = st.size;
        cut_fd((int)fd, sz, 0);
        uaos_close((int)fd);
    }
    return 0;
}
