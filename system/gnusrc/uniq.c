/* uniq.c — GNU coreutils 'uniq' for UAOS gnu: layer
 *
 * Remove duplicate adjacent lines.
 *   uniq [OPTION]... [INPUT [OUTPUT]]
 * Options: -c, --count, -d, --repeated, -u, --unique, -i, --ignore-case,
 *          -f N, --skip-fields=N, -s N, --skip-chars=N, -w N, --check-chars=N
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_count = 0;
static int opt_repeated = 0;
static int opt_unique = 0;
static int opt_ignore_case = 0;
static int opt_skip_fields = 0;
static int opt_skip_chars = 0;
static int opt_check_chars = 0;

static int lines_equal(const char *a, int alen, const char *b, int blen)
{
    /* Apply skip fields + skip chars to get comparison start */
    int ai = 0, bi = 0;
    for (int f = 0; f < opt_skip_fields; f++) {
        while (ai < alen && a[ai] == ' ') ai++;
        while (ai < alen && a[ai] != ' ') ai++;
        while (bi < blen && b[bi] == ' ') bi++;
        while (bi < blen && b[bi] != ' ') bi++;
    }
    ai += opt_skip_chars;
    bi += opt_skip_chars;
    int max_check = opt_check_chars;
    int checked = 0;
    while (ai < alen && bi < blen) {
        char ac = a[ai++], bc = b[bi++];
        if (opt_ignore_case) {
            if (ac >= 'A' && ac <= 'Z') ac += 32;
            if (bc >= 'A' && bc <= 'Z') bc += 32;
        }
        if (ac != bc) return 0;
        checked++;
        if (max_check > 0 && checked >= max_check) break;
    }
    if (max_check > 0) return 1;
    return (ai >= alen && bi >= blen);
}

static void output_line(long out_fd, const char *line, int len, int count)
{
    if (opt_count) {
        char num[12];
        int_to_dec(count, num, sizeof(num));
        int slen = (int)uaos_strlen(num);
        /* pad to 7 chars */
        for (int i = slen; i < 7; i++) {
            if (out_fd >= 0) uaos_write_file((int)out_fd, (const uint8_t *)" ", 1);
            else put_c(' ');
        }
        if (out_fd >= 0) uaos_write_file((int)out_fd, (const uint8_t *)num, slen);
        else put_s(num);
        if (out_fd >= 0) uaos_write_file((int)out_fd, (const uint8_t *)" ", 1);
        else put_c(' ');
    }
    if (out_fd >= 0) {
        uaos_write_file((int)out_fd, (const uint8_t *)line, len);
        uaos_write_file((int)out_fd, (const uint8_t *)"\n", 1);
    } else {
        uaos_write(1, line, len);
        put_c('\n');
    }
}

static void uniq_fd(int in_fd, uint32_t sz, int is_stdin, long out_fd)
{
    char prev[UAOS_CMD_LINE_MAX * 2];
    char cur[UAOS_CMD_LINE_MAX * 2];
    int prev_len = -1;
    int prev_valid = 0;
    int dup_count = 0;
    int col = 0;
    uint32_t pos = 0;

    for (;;) {
        uint8_t ch;
        long n;
        if (is_stdin) { n = uaos_read(in_fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file(in_fd, &ch, 1); pos++; }
        if (n <= 0) break;

        if (ch == '\n') {
            cur[col] = '\0';
            int cur_len = col;
            col = 0;

            if (prev_valid && lines_equal(prev, prev_len, cur, cur_len)) {
                dup_count++;
            } else {
                if (prev_valid) {
                    int total = dup_count + 1;
                    int print = 0;
                    if (opt_repeated && total > 1) print = 1;
                    else if (opt_unique && total == 1) print = 1;
                    else if (!opt_repeated && !opt_unique) print = 1;
                    if (print) output_line(out_fd, prev, prev_len, total);
                }
                uaos_memcpy(prev, cur, cur_len + 1);
                prev_len = cur_len;
                prev_valid = 1;
                dup_count = 0;
            }
        } else if (ch != '\r') {
            if (col < (int)sizeof(cur) - 1) cur[col++] = (char)ch;
        }
    }
    /* flush last group */
    if (col > 0) {
        cur[col] = '\0';
        if (prev_valid && lines_equal(prev, prev_len, cur, col)) {
            dup_count++;
        } else {
            if (prev_valid) {
                int total = dup_count + 1;
                int print = 0;
                if (opt_repeated && total > 1) print = 1;
                else if (opt_unique && total == 1) print = 1;
                else if (!opt_repeated && !opt_unique) print = 1;
                if (print) output_line(out_fd, prev, prev_len, total);
            }
            uaos_memcpy(prev, cur, col + 1);
            prev_len = col;
            prev_valid = 1;
            dup_count = 0;
        }
    }
    if (prev_valid) {
        int total = dup_count + 1;
        int print = 0;
        if (opt_repeated && total > 1) print = 1;
        else if (opt_unique && total == 1) print = 1;
        else if (!opt_repeated && !opt_unique) print = 1;
        if (print) output_line(out_fd, prev, prev_len, total);
    }
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"count",       'c', no_argument},
        {"repeated",    'd', no_argument},
        {"unique",      'u', no_argument},
        {"ignore-case", 'i', no_argument},
        {"skip-fields", 'f', required_argument},
        {"skip-chars",  's', required_argument},
        {"check-chars", 'w', required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "cduif:s:w:", long_opts, &li)) != -1) {
        switch (opt) {
            case 'c': opt_count = 1; break;
            case 'd': opt_repeated = 1; break;
            case 'u': opt_unique = 1; break;
            case 'i': opt_ignore_case = 1; break;
            case 'f': { long v; if (uaos_optarg_long(&v)) opt_skip_fields = (int)v; } break;
            case 's': { long v; if (uaos_optarg_long(&v)) opt_skip_chars = (int)v; } break;
            case 'w': { long v; if (uaos_optarg_long(&v)) opt_check_chars = (int)v; } break;
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    const char *infile = (nops >= 1) ? uaos_operand(argc, argv, 0) : "-";
    const char *outfile = (nops >= 2) ? uaos_operand(argc, argv, 1) : NULL;

    long out_fd = -1;
    if (outfile) {
        char path[UAOS_CMD_PATH_MAX];
        cmd_make_abs(outfile, path, sizeof(path));
        out_fd = uaos_open(path, UAOS_O_WRONLY | UAOS_O_CREAT | UAOS_O_TRUNC);
        if (out_fd < 0) { put_s("uniq: cannot open: "); put_line(outfile); return 1; }
    }

    if (infile[0] == '-' && infile[1] == '\0') {
        uniq_fd(0, 0, 1, out_fd);
    } else {
        char path[UAOS_CMD_PATH_MAX];
        cmd_make_abs(infile, path, sizeof(path));
        long fd = uaos_open(path, UAOS_O_RDONLY);
        if (fd < 0) { put_s("uniq: "); put_s(infile); put_line(": No such file"); return 1; }
        struct uaos_stat st; uint32_t sz = 0;
        if (uaos_stat(path, &st) == 0) sz = st.size;
        uniq_fd((int)fd, sz, 0, out_fd);
        uaos_close((int)fd);
    }
    if (out_fd >= 0) uaos_close((int)out_fd);
    return 0;
}
