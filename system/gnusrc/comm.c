/* comm.c — GNU coreutils 'comm' for UAOS gnu: layer
 *
 * Compare two sorted files line by line.
 *   comm [OPTION]... FILE1 FILE2
 * Options: -1, -2, -3 (suppress columns), --check-order, --nocheck-order,
 *          --output-delimiter=STR
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_suppress1 = 0;
static int opt_suppress2 = 0;
static int opt_suppress3 = 0;
static char opt_delim[16] = "\t";

static int read_line(int fd, uint32_t sz, uint32_t *pos, int is_stdin, char *out, int max)
{
    int col = 0;
    for (;;) {
        uint8_t ch;
        long n;
        if (is_stdin) { n = uaos_read(fd, &ch, 1); }
        else { if (*pos >= sz) { if (col == 0) return -1; break; } n = uaos_read_file(fd, &ch, 1); (*pos)++; }
        if (n <= 0) { if (col == 0) return -1; break; }
        if (ch == '\n') break;
        if (ch != '\r' && col < max - 1) out[col++] = (char)ch;
    }
    out[col] = '\0';
    return col;
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"output-delimiter", 0, required_argument},
        {"check-order",      0, no_argument},
        {"nocheck-order",    0, no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "123", long_opts, &li)) != -1) {
        switch (opt) {
            case '1': opt_suppress1 = 1; break;
            case '2': opt_suppress2 = 1; break;
            case '3': opt_suppress3 = 1; break;
            case UAOS_GO_LONG + 0:
                if (g_optarg) {
                    int i = 0;
                    while (g_optarg[i] && i < (int)sizeof(opt_delim) - 1) {
                        opt_delim[i] = g_optarg[i]; i++;
                    }
                    opt_delim[i] = '\0';
                }
                break;
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    if (nops < 2) { put_line("comm: two file operands required"); return 1; }

    const char *f1 = uaos_operand(argc, argv, 0);
    const char *f2 = uaos_operand(argc, argv, 1);

    int is_stdin1 = (f1[0] == '-' && f1[1] == '\0');
    int is_stdin2 = (f2[0] == '-' && f2[1] == '\0');

    long fd1, fd2;
    uint32_t sz1 = 0, sz2 = 0, pos1 = 0, pos2 = 0;
    if (is_stdin1) fd1 = 0;
    else {
        char p1[UAOS_CMD_PATH_MAX]; cmd_make_abs(f1, p1, sizeof(p1));
        fd1 = uaos_open(p1, UAOS_O_RDONLY);
        if (fd1 < 0) { put_s("comm: "); put_s(f1); put_line(": No such file"); return 1; }
        struct uaos_stat st; if (uaos_stat(p1, &st) == 0) sz1 = st.size;
    }
    if (is_stdin2) fd2 = 0;
    else {
        char p2[UAOS_CMD_PATH_MAX]; cmd_make_abs(f2, p2, sizeof(p2));
        fd2 = uaos_open(p2, UAOS_O_RDONLY);
        if (fd2 < 0) { put_s("comm: "); put_s(f2); put_line(": No such file"); return 1; }
        struct uaos_stat st; if (uaos_stat(p2, &st) == 0) sz2 = st.size;
    }

    char l1[UAOS_CMD_LINE_MAX * 2], l2[UAOS_CMD_LINE_MAX * 2];
    int r1 = read_line((int)fd1, sz1, &pos1, is_stdin1, l1, sizeof(l1));
    int r2 = read_line((int)fd2, sz2, &pos2, is_stdin2, l2, sizeof(l2));

    while (r1 >= 0 && r2 >= 0) {
        int cmp = uaos_strcmp(l1, l2);
        if (cmp < 0) {
            if (!opt_suppress1) { put_s(l1); put_c('\n'); }
            r1 = read_line((int)fd1, sz1, &pos1, is_stdin1, l1, sizeof(l1));
        } else if (cmp > 0) {
            if (!opt_suppress2) { put_s(opt_delim); put_s(l2); put_c('\n'); }
            r2 = read_line((int)fd2, sz2, &pos2, is_stdin2, l2, sizeof(l2));
        } else {
            if (!opt_suppress3) { put_s(opt_delim); put_s(opt_delim); put_s(l1); put_c('\n'); }
            r1 = read_line((int)fd1, sz1, &pos1, is_stdin1, l1, sizeof(l1));
            r2 = read_line((int)fd2, sz2, &pos2, is_stdin2, l2, sizeof(l2));
        }
    }
    while (r1 >= 0) {
        if (!opt_suppress1) { put_s(l1); put_c('\n'); }
        r1 = read_line((int)fd1, sz1, &pos1, is_stdin1, l1, sizeof(l1));
    }
    while (r2 >= 0) {
        if (!opt_suppress2) { put_s(opt_delim); put_s(l2); put_c('\n'); }
        r2 = read_line((int)fd2, sz2, &pos2, is_stdin2, l2, sizeof(l2));
    }

    if (fd1 > 0) uaos_close((int)fd1);
    if (fd2 > 0) uaos_close((int)fd2);
    return 0;
}
