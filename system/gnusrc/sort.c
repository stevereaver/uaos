/* sort.c — GNU coreutils 'sort' for UAOS gnu: layer
 *
 * Sort lines of text files.
 *   sort [OPTION]... [FILE]...
 * Options: -n, --numeric-sort, -r, --reverse, -u, --unique,
 *          -f, --ignore-case, -o FILE, --output=FILE, -t CHAR, -k POS
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

#define SORT_MAX_LINES 2048
#define SORT_LINE_MAX  (UAOS_CMD_LINE_MAX * 2)

static int  opt_numeric = 0;
static int  opt_reverse = 0;
static int  opt_unique = 0;
static int  opt_ignore_case = 0;
static char opt_field_sep = 0;
static int  opt_key = 0;

static int cmp_ci(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) {
        char ac = a[i], bc = b[i];
        if (ac >= 'A' && ac <= 'Z') ac += 32;
        if (bc >= 'A' && bc <= 'Z') bc += 32;
        if (ac != bc) return ac - bc;
        i++;
    }
    return (unsigned char)a[i] - (unsigned char)b[i];
}

static int cmp_num(const char *a, const char *b)
{
    double na = 0, nb = 0;
    int ha = 0, hb = 0;
    const char *pa = a, *pb = b;
    while (*pa && (*pa < '0' || *pa > '9') && *pa != '-' && *pa != '.') pa++;
    while (*pb && (*pb < '0' || *pb > '9') && *pb != '-' && *pb != '.') pb++;
    int sgn_a = 1, sgn_b = 1;
    if (*pa == '-') { sgn_a = -1; pa++; }
    if (*pb == '-') { sgn_b = -1; pb++; }
    while (*pa >= '0' && *pa <= '9') { na = na * 10 + (*pa - '0'); ha = 1; pa++; }
    while (*pb >= '0' && *pb <= '9') { nb = nb * 10 + (*pb - '0'); hb = 1; pb++; }
    if (ha && hb) {
        long va = (long)(na * sgn_a), vb = (long)(nb * sgn_b);
        if (va != vb) return va < vb ? -1 : 1;
    }
    return cmp_ci(a, b);
}

static int sort_cmp(const char *a, const char *b)
{
    int r;
    if (opt_numeric) r = cmp_num(a, b);
    else if (opt_ignore_case) r = cmp_ci(a, b);
    else r = uaos_strcmp(a, b);
    if (opt_reverse) r = -r;
    return r;
}

static void sort_fd(int fd, uint32_t sz, int is_stdin,
                    char (*lines)[SORT_LINE_MAX], int *count)
{
    int col = 0;
    uint32_t pos = 0;
    for (;;) {
        uint8_t ch;
        long n;
        if (is_stdin) { n = uaos_read(fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
        if (n <= 0) break;
        if (ch == '\n') {
            if (*count < SORT_MAX_LINES) {
                lines[*count][col] = '\0';
                (*count)++;
            }
            col = 0;
        } else if (ch != '\r') {
            if (col < SORT_LINE_MAX - 1) lines[*count][col++] = (char)ch;
        }
    }
    if (col > 0 && *count < SORT_MAX_LINES) {
        lines[*count][col] = '\0';
        (*count)++;
    }
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"numeric-sort", 'n', no_argument},
        {"reverse",      'r', no_argument},
        {"unique",       'u', no_argument},
        {"ignore-case",  'f', no_argument},
        {"output",       'o', required_argument},
        {"field-separator",'t', required_argument},
        {"key",          'k', required_argument},
        {NULL, 0, 0}
    };
    const char *output_file = NULL;
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "nrufo:t:k:", long_opts, &li)) != -1) {
        switch (opt) {
            case 'n': opt_numeric = 1; break;
            case 'r': opt_reverse = 1; break;
            case 'u': opt_unique = 1; break;
            case 'f': opt_ignore_case = 1; break;
            case 'o': output_file = g_optarg; break;
            case 't': if (g_optarg) opt_field_sep = g_optarg[0]; break;
            case 'k': { long v; if (uaos_optarg_long(&v)) opt_key = (int)v; } break;
            default:  return 1;
        }
    }

    char (*lines)[SORT_LINE_MAX] = (char (*)[SORT_LINE_MAX])
        uaos_alloc((long)(SORT_MAX_LINES * SORT_LINE_MAX));
    if (!lines) { put_line("sort: out of memory"); return 1; }
    int count = 0;

    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) {
        sort_fd(0, 0, 1, lines, &count);
    } else {
        for (int i = 0; i < nfiles; i++) {
            const char *fname = uaos_operand(argc, argv, i);
            if (!fname) continue;
            if (fname[0] == '-' && fname[1] == '\0') { sort_fd(0, 0, 1, lines, &count); continue; }
            char path[UAOS_CMD_PATH_MAX];
            cmd_make_abs(fname, path, sizeof(path));
            long fd = uaos_open(path, UAOS_O_RDONLY);
            if (fd < 0) { put_s("sort: "); put_s(fname); put_line(": No such file"); continue; }
            struct uaos_stat st; uint32_t sz = 0;
            if (uaos_stat(path, &st) == 0) sz = st.size;
            sort_fd((int)fd, sz, 0, lines, &count);
            uaos_close((int)fd);
        }
    }

    /* bubble sort */
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - 1 - i; j++) {
            if (sort_cmp(lines[j], lines[j + 1]) > 0) {
                char tmp[SORT_LINE_MAX];
                uaos_strcpy(tmp, lines[j]);
                uaos_strcpy(lines[j], lines[j + 1]);
                uaos_strcpy(lines[j + 1], tmp);
            }
        }
    }

    /* output */
    long out_fd = 1;
    if (output_file) {
        char path[UAOS_CMD_PATH_MAX];
        cmd_make_abs(output_file, path, sizeof(path));
        out_fd = uaos_open(path, UAOS_O_WRONLY | UAOS_O_CREAT | UAOS_O_TRUNC);
        if (out_fd < 0) { put_s("sort: cannot open: "); put_line(output_file); return 1; }
    }

    const char *prev = NULL;
    for (int i = 0; i < count; i++) {
        if (opt_unique && prev && sort_cmp(prev, lines[i]) == 0) continue;
        if (out_fd == 1) {
            put_s(lines[i]);
            put_c('\n');
        } else {
            uaos_write_file((int)out_fd, (const uint8_t *)lines[i], (long)uaos_strlen(lines[i]));
            uaos_write_file((int)out_fd, (const uint8_t *)"\n", 1);
        }
        prev = lines[i];
    }
    if (out_fd != 1) uaos_close((int)out_fd);
    return 0;
}
