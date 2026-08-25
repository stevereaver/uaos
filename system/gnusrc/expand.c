/* expand.c — GNU coreutils 'expand' for UAOS gnu: layer
 *
 * Convert tabs to spaces.
 *   expand [OPTION]... [FILE]...
 * Options: -i, --initial, -t LIST, --tabs=LIST
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int  opt_initial = 0;
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

static int next_tab(int col)
{
    if (tab_count == 0) {
        return ((col / tab_default) + 1) * tab_default;
    }
    if (tab_count == 1) {
        int t = tab_stops[0];
        return ((col / t) + 1) * t;
    }
    for (int i = 0; i < tab_count; i++) {
        if (tab_stops[i] > col) return tab_stops[i];
    }
    return col + 1;
}

static void expand_fd(int fd, uint32_t sz, int is_stdin)
{
    int col = 0;
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
            col = 0; seen_non_ws = 0;
            continue;
        }

        if (ch == '\t') {
            if (opt_initial && seen_non_ws) {
                put_c('\t');
            } else {
                int target = next_tab(col);
                while (col < target) { put_c(' '); col++; }
            }
            continue;
        }

        if (ch != ' ') seen_non_ws = 1;
        put_c((char)ch);
        col++;
    }
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"initial", 'i', no_argument},
        {"tabs",    't', required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "it:", long_opts, &li)) != -1) {
        switch (opt) {
            case 'i': opt_initial = 1; break;
            case 't': if (g_optarg) parse_tabs(g_optarg); break;
            default:  return 1;
        }
    }

    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) { expand_fd(0, 0, 1); return 0; }
    for (int i = 0; i < nfiles; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (!fname) continue;
        if (fname[0] == '-' && fname[1] == '\0') { expand_fd(0, 0, 1); continue; }
        char path[UAOS_CMD_PATH_MAX];
        cmd_make_abs(fname, path, sizeof(path));
        long fd = uaos_open(path, UAOS_O_RDONLY);
        if (fd < 0) { put_s("expand: "); put_s(fname); put_line(": No such file"); continue; }
        struct uaos_stat st; uint32_t sz = 0;
        if (uaos_stat(path, &st) == 0) sz = st.size;
        expand_fd((int)fd, sz, 0);
        uaos_close((int)fd);
    }
    return 0;
}
