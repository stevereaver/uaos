/* head.c — GNU coreutils 'head' for UAOS gnu: layer
 *
 * Output the first part of files.
 *   head [OPTION]... [FILE]...
 * Options: -n N, --lines=N, -c N, --bytes=N, -q, --quiet, -v, --verbose
 * N prefixes: -n -5 = all but last 5 lines, -n +5 = from line 5 onward.
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static long opt_count = 10;
static int  opt_mode  = 0;  /* 0=lines, 1=bytes */
static int  opt_sign  = 1;  /* +1 = first N, -1 = all but last N */
static int  opt_quiet = 0;
static int  opt_verbose = 0;

static void parse_count(const char *arg)
{
    if (!arg) return;
    const char *p = arg;
    if (*p == '+') { opt_sign = 1; p++; }
    else if (*p == '-') { opt_sign = -1; p++; }
    long v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    /* skip optional suffix (b, k, m) */
    opt_count = v;
}

static void head_fd(int fd, uint32_t sz, int is_stdin)
{
    uint32_t pos = 0;
    if (opt_mode == 1) {
        /* byte mode */
        if (opt_sign > 0) {
            long remaining = opt_count;
            while (remaining > 0) {
                uint8_t ch;
                long n;
                if (is_stdin) { n = uaos_read(fd, &ch, 1); }
                else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
                if (n <= 0) break;
                put_c((char)ch);
                remaining--;
            }
        } else {
            /* all but last N bytes — need to buffer */
            long skip = opt_count;
            long total = is_stdin ? 0 : (long)sz;
            if (is_stdin) {
                /* read all into memory */
                uint8_t *buf = (uint8_t *)uaos_alloc(65536);
                if (!buf) return;
                long total_read = 0;
                for (;;) {
                    long n = uaos_read(fd, buf + total_read, 65536 - total_read);
                    if (n <= 0) break;
                    total_read += n;
                    if (total_read >= 65536) break;
                }
                total = total_read;
                long print = total - skip;
                if (print < 0) print = 0;
                uaos_write(1, buf, print);
                return;
            }
            long print = total - skip;
            if (print < 0) print = 0;
            while (print > 0 && pos < sz) {
                uint8_t ch;
                uaos_read_file(fd, &ch, 1); pos++;
                put_c((char)ch);
                print--;
            }
        }
        return;
    }

    /* line mode */
    if (opt_sign > 0) {
        long remaining = opt_count;
        long line_no = 0;
        while (remaining > 0) {
            uint8_t ch;
            long n;
            if (is_stdin) { n = uaos_read(fd, &ch, 1); }
            else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
            if (n <= 0) break;
            put_c((char)ch);
            if (ch == '\n') {
                line_no++;
                if (opt_sign > 0 && opt_count > 0) remaining--;
                if (remaining <= 0 && opt_count > 0) break;
            }
        }
        /* for +N mode, print from line N */
        if (opt_count == 0) return;
    } else {
        /* all but last N lines — need to know total lines or use ring buffer */
        /* Simple approach: read all lines into memory, print all but last N */
        char (*lines)[UAOS_CMD_LINE_MAX] = (char (*)[UAOS_CMD_LINE_MAX])
            uaos_alloc((long)(4096 * UAOS_CMD_LINE_MAX));
        if (!lines) return;
        int *lens = (int *)uaos_alloc(4096 * sizeof(int));
        if (!lens) return;
        int count = 0, col = 0;
        for (;;) {
            uint8_t ch;
            long n;
            if (is_stdin) { n = uaos_read(fd, &ch, 1); }
            else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
            if (n <= 0) break;
            if (ch == '\n') {
                if (count < 4096) { lines[count][col] = '\0'; lens[count] = col; count++; }
                col = 0;
            } else if (ch != '\r') {
                if (col < UAOS_CMD_LINE_MAX - 1) lines[count][col++] = (char)ch;
            }
        }
        if (col > 0 && count < 4096) { lines[count][col] = '\0'; lens[count] = col; count++; }
        long start = count - opt_count;
        if (start < 0) start = 0;
        for (int i = (int)start; i < count; i++) {
            uaos_write(1, lines[i], lens[i]);
            put_c('\n');
        }
    }
}

static void head_file(const char *fname, int multi)
{
    if (opt_verbose || (multi && !opt_quiet)) {
        put_s("==> ");
        put_s(fname);
        put_line(" <==");
    }
    if (fname[0] == '-' && fname[1] == '\0') {
        head_fd(0, 0, 1);
        return;
    }
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(fname, path, sizeof(path));
    long fd = uaos_open(path, UAOS_O_RDONLY);
    if (fd < 0) {
        put_s("head: cannot open '");
        put_s(fname);
        put_line("' for reading");
        return;
    }
    struct uaos_stat st;
    uint32_t sz = 0;
    if (uaos_stat(path, &st) == 0) sz = st.size;
    head_fd((int)fd, sz, 0);
    uaos_close((int)fd);
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"lines",   'n', required_argument},
        {"bytes",   'c', required_argument},
        {"quiet",   'q', no_argument},
        {"verbose", 'v', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "n:c:qv", long_opts, &li)) != -1) {
        switch (opt) {
            case 'n': opt_mode = 0; parse_count(g_optarg); break;
            case 'c': opt_mode = 1; parse_count(g_optarg); break;
            case 'q': opt_quiet = 1; break;
            case 'v': opt_verbose = 1; break;
            default:  return 1;
        }
    }

    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) {
        head_fd(0, 0, 1);
        return 0;
    }
    for (int i = 0; i < nfiles; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (fname) head_file(fname, nfiles > 1);
    }
    return 0;
}
