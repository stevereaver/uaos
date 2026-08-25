/* unexpand.c — GNU coreutils 'unexpand' for UAOS gnu: layer
 *
 * Convert spaces to tabs.
 *   unexpand [OPTION]... [FILE]...
 * Options: -a, --all, -f, --first-only, -t LIST, --tabs=LIST
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int  opt_all = 0;
static int  opt_first_only = 0;
static int  tab_stops[16];
static int  tab_count = 0;
static int  tab_default = 8;

static void parse_tabs(const char *spec)
{
    tab_count = 0;
    const char *p = spec;
    while (*p && tab_count < 16) {
        int v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
        if (v > 0) tab_stops[tab_count++] = v;
        while (*p == ',' || *p == ' ') p++;
    }
}

static int is_tab_stop(int col)
{
    if (tab_count == 0) return (col % tab_default) == 0;
    if (tab_count == 1) return (col % tab_stops[0]) == 0;
    for (int i = 0; i < tab_count; i++) if (tab_stops[i] == col) return 1;
    return 0;
}

static int next_tab_stop(int col)
{
    if (tab_count == 0) return ((col / tab_default) + 1) * tab_default;
    if (tab_count == 1) return ((col / tab_stops[0]) + 1) * tab_stops[0];
    for (int i = 0; i < tab_count; i++) if (tab_stops[i] > col) return tab_stops[i];
    return -1;
}

static void unexpand_fd(int fd, uint32_t sz, int is_stdin)
{
    int col = 0;
    int run_start = 0;
    int seen_non_ws = 0;
    uint32_t pos = 0;

    for (;;) {
        uint8_t ch;
        long n;
        if (is_stdin) { n = uaos_read(fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
        if (n <= 0) break;

        if (ch == '\n') {
            put_c('\n');
            col = 0; run_start = 0; seen_non_ws = 0;
            continue;
        }

        if (ch == ' ') {
            col++;
            /* check if we can replace run of spaces from run_start to col with tabs */
            if (is_tab_stop(col)) {
                if (opt_all || (!seen_non_ws && !opt_first_only) || (!seen_non_ws && opt_first_only)) {
                    /* emit tabs for the run */
                    int c = run_start;
                    while (c < col) {
                        int next = next_tab_stop(c);
                        if (next > 0 && next <= col) {
                            put_c('\t');
                            c = next;
                        } else {
                            while (c < col) { put_c(' '); c++; }
                        }
                    }
                    run_start = col;
                }
            }
            continue;
        }

        /* flush any pending spaces */
        while (run_start < col) { put_c(' '); run_start++; }
        run_start = col + 1;

        if (ch != ' ') seen_non_ws = 1;
        put_c((char)ch);
        col++;
        run_start = col;
    }
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"all",       'a', no_argument},
        {"first-only",'f', no_argument},
        {"tabs",      't', required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "aft:", long_opts, &li)) != -1) {
        switch (opt) {
            case 'a': opt_all = 1; break;
            case 'f': opt_first_only = 1; break;
            case 't': if (g_optarg) parse_tabs(g_optarg); break;
            default:  return 1;
        }
    }

    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) { unexpand_fd(0, 0, 1); return 0; }
    for (int i = 0; i < nfiles; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (!fname) continue;
        if (fname[0] == '-' && fname[1] == '\0') { unexpand_fd(0, 0, 1); continue; }
        char path[UAOS_CMD_PATH_MAX];
        cmd_make_abs(fname, path, sizeof(path));
        long fd = uaos_open(path, UAOS_O_RDONLY);
        if (fd < 0) { put_s("unexpand: "); put_s(fname); put_line(": No such file"); continue; }
        struct uaos_stat st; uint32_t sz = 0;
        if (uaos_stat(path, &st) == 0) sz = st.size;
        unexpand_fd((int)fd, sz, 0);
        uaos_close((int)fd);
    }
    return 0;
}
