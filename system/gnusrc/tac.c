/* tac.c — GNU coreutils 'tac' for UAOS gnu: layer
 *
 * Concatenate and print files in reverse (line by line).
 *   tac [OPTION]... [FILE]...
 * Options: -b, --before, -s, --separator=STRING
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

#define TAC_MAX_LINES 2048
#define TAC_LINE_MAX  UAOS_CMD_LINE_MAX

static int opt_before = 0;
static char opt_sep[4] = "\n";

static void tac_fd(int fd, uint32_t sz, int is_stdin)
{
    /* Read entire file into memory, split into lines, print in reverse. */
    char (*lines)[TAC_LINE_MAX] = (char (*)[TAC_LINE_MAX])
        uaos_alloc((long)(TAC_MAX_LINES * TAC_LINE_MAX));
    if (!lines) { put_line("tac: out of memory"); return; }
    int *line_lens = (int *)uaos_alloc((long)(TAC_MAX_LINES * sizeof(int)));
    if (!line_lens) { put_line("tac: out of memory"); return; }

    int count = 0;
    int col = 0;
    uint32_t pos = 0;
    for (;;) {
        uint8_t ch;
        long n;
        if (is_stdin) { n = uaos_read(fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
        if (n <= 0) break;

        if (count >= TAC_MAX_LINES) break;
        if (ch == (uint8_t)opt_sep[0]) {
            lines[count][col] = '\0';
            line_lens[count] = col;
            count++;
            col = 0;
        } else {
            if (col < TAC_LINE_MAX - 1) lines[count][col++] = (char)ch;
        }
    }
    if (col > 0 && count < TAC_MAX_LINES) {
        lines[count][col] = '\0';
        line_lens[count] = col;
        count++;
    }

    /* Print in reverse. */
    for (int i = count - 1; i >= 0; i--) {
        if (opt_before) {
            put_s(opt_sep);
            put_s(lines[i]);
        } else {
            put_s(lines[i]);
            put_s(opt_sep);
        }
    }
}

static void tac_file(const char *fname)
{
    if (fname[0] == '-' && fname[1] == '\0') {
        tac_fd(0, 0, 1);
        return;
    }
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(fname, path, sizeof(path));
    long fd = uaos_open(path, UAOS_O_RDONLY);
    if (fd < 0) {
        put_s("tac: ");
        put_s(fname);
        put_line(": No such file or directory");
        return;
    }
    struct uaos_stat st;
    uint32_t sz = 0;
    if (uaos_stat(path, &st) == 0) sz = st.size;
    tac_fd((int)fd, sz, 0);
    uaos_close((int)fd);
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"before",    'b', no_argument},
        {"separator", 's', required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "bs:", long_opts, &li)) != -1) {
        switch (opt) {
            case 'b': opt_before = 1; break;
            case 's':
                if (g_optarg && g_optarg[0])
                    opt_sep[0] = g_optarg[0];
                break;
            default: return 1;
        }
    }

    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) {
        tac_fd(0, 0, 1);
        return 0;
    }
    for (int i = 0; i < nfiles; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (fname) tac_file(fname);
    }
    return 0;
}
