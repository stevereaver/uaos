/* paste.c — GNU coreutils 'paste' for UAOS gnu: layer
 *
 * Merge lines of files.
 *   paste [OPTION]... [FILE]...
 * Options: -d LIST, --delimiters=LIST, -s, --serial
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static char opt_delims[32] = "\t";
static int  opt_serial = 0;

static void paste_parallel(int nfiles, const char **fnames)
{
    /* Open all files */
    long fds[16];
    uint32_t szs[16];
    uint32_t poss[16];
    int active = 0;
    for (int i = 0; i < nfiles && i < 16; i++) {
        if (fnames[i][0] == '-' && fnames[i][1] == '\0') {
            fds[i] = 0; szs[i] = 0; poss[i] = 0; active++; continue;
        }
        char path[UAOS_CMD_PATH_MAX];
        cmd_make_abs(fnames[i], path, sizeof(path));
        fds[i] = uaos_open(path, UAOS_O_RDONLY);
        if (fds[i] < 0) { fds[i] = -1; continue; }
        struct uaos_stat st; szs[i] = 0;
        if (uaos_stat(path, &st) == 0) szs[i] = st.size;
        poss[i] = 0;
        active++;
    }
    if (active == 0) return;

    int delim_len = (int)uaos_strlen(opt_delims);
    if (delim_len == 0) { opt_delims[0] = '\t'; delim_len = 1; }

    for (;;) {
        int any_data = 0;
        for (int i = 0; i < nfiles && i < 16; i++) {
            if (fds[i] < 0) {
                /* file that failed to open — just emit delimiter */
                if (i > 0) put_c(opt_delims[(i - 1) % delim_len]);
                continue;
            }
            if (i > 0) put_c(opt_delims[(i - 1) % delim_len]);

            int is_stdin = (fds[i] == 0);
            int got_line = 0;
            for (;;) {
                uint8_t ch;
                long n;
                if (is_stdin) { n = uaos_read((int)fds[i], &ch, 1); }
                else { if (poss[i] >= szs[i]) { n = 0; } else { n = uaos_read_file((int)fds[i], &ch, 1); poss[i]++; } }
                if (n <= 0) break;
                if (ch == '\n') { got_line = 1; break; }
                if (ch != '\r') put_c((char)ch);
                any_data = 1;
            }
            (void)got_line;
        }
        put_c('\n');
        if (!any_data) break;
    }

    for (int i = 0; i < nfiles && i < 16; i++) {
        if (fds[i] > 0) uaos_close((int)fds[i]);
    }
}

static void paste_serial(const char *fname)
{
    int is_stdin = (fname[0] == '-' && fname[1] == '\0');
    long fd;
    uint32_t sz = 0, pos = 0;
    if (is_stdin) { fd = 0; }
    else {
        char path[UAOS_CMD_PATH_MAX];
        cmd_make_abs(fname, path, sizeof(path));
        fd = uaos_open(path, UAOS_O_RDONLY);
        if (fd < 0) { put_s("paste: "); put_s(fname); put_line(": No such file"); return; }
        struct uaos_stat st;
        if (uaos_stat(path, &st) == 0) sz = st.size;
    }

    int first = 1;
    for (;;) {
        uint8_t ch;
        long n;
        if (is_stdin) { n = uaos_read((int)fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file((int)fd, &ch, 1); pos++; }
        if (n <= 0) break;
        if (ch == '\n') {
            put_c('\t');
            first = 0;
        } else if (ch != '\r') {
            put_c((char)ch);
        }
    }
    put_c('\n');
    if (!is_stdin) uaos_close((int)fd);
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"delimiters", 'd', required_argument},
        {"serial",     's', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "d:s", long_opts, &li)) != -1) {
        switch (opt) {
            case 'd':
                if (g_optarg) {
                    int i = 0, j = 0;
                    while (g_optarg[i] && j < (int)sizeof(opt_delims) - 1) {
                        if (g_optarg[i] == '\\' && g_optarg[i+1]) {
                            i++;
                            switch (g_optarg[i]) {
                                case 'n': opt_delims[j++] = '\n'; break;
                                case 't': opt_delims[j++] = '\t'; break;
                                case 'r': opt_delims[j++] = '\r'; break;
                                case '\\': opt_delims[j++] = '\\'; break;
                                case '0': opt_delims[j++] = '\0'; break;
                                default: opt_delims[j++] = g_optarg[i]; break;
                            }
                        } else {
                            opt_delims[j++] = g_optarg[i];
                        }
                        i++;
                    }
                    opt_delims[j] = '\0';
                }
                break;
            case 's': opt_serial = 1; break;
            default:  return 1;
        }
    }

    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) {
        const char *stdin_name = "-";
        if (opt_serial) paste_serial(stdin_name);
        else paste_parallel(1, &stdin_name);
        return 0;
    }

    if (opt_serial) {
        for (int i = 0; i < nfiles; i++) {
            const char *fname = uaos_operand(argc, argv, i);
            if (fname) paste_serial(fname);
        }
    } else {
        /* collect filenames */
        const char *fnames[16];
        for (int i = 0; i < nfiles && i < 16; i++)
            fnames[i] = uaos_operand(argc, argv, i);
        paste_parallel(nfiles < 16 ? nfiles : 16, fnames);
    }
    return 0;
}
