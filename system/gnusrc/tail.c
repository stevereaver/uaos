/* tail.c — GNU coreutils 'tail' for UAOS gnu: layer
 *
 * Output the last part of files.
 *   tail [OPTION]... [FILE]...
 * Options: -n N, --lines=N, -c N, --bytes=N, -q, --quiet, -v, --verbose
 * N prefixes: -n +5 = from line 5 onward, -n 5 = last 5 lines.
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static long opt_count = 10;
static int  opt_mode  = 0;  /* 0=lines, 1=bytes */
static int  opt_sign  = 0;  /* 0 = last N, 1 = from +N onward */
static int  opt_quiet = 0;
static int  opt_verbose = 0;

static void parse_count(const char *arg)
{
    if (!arg) return;
    const char *p = arg;
    if (*p == '+') { opt_sign = 1; p++; }
    else { opt_sign = 0; if (*p == '-') p++; }
    long v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    opt_count = v;
}

static void tail_fd(int fd, uint32_t sz, int is_stdin)
{
    uint32_t pos = 0;

    if (opt_mode == 1) {
        /* byte mode */
        if (opt_sign) {
            /* from byte N onward */
            long skip = opt_count - 1;
            if (skip < 0) skip = 0;
            while (skip > 0) {
                uint8_t ch;
                if (is_stdin) { if (uaos_read(fd, &ch, 1) <= 0) break; }
                else { if (pos >= sz) break; uaos_read_file(fd, &ch, 1); pos++; }
                skip--;
            }
            for (;;) {
                uint8_t ch;
                long n;
                if (is_stdin) { n = uaos_read(fd, &ch, 1); }
                else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
                if (n <= 0) break;
                put_c((char)ch);
            }
        } else {
            /* last N bytes */
            long total = is_stdin ? 0 : (long)sz;
            uint8_t *buf = NULL;
            if (is_stdin) {
                buf = (uint8_t *)uaos_alloc(65536);
                if (!buf) return;
                long total_read = 0;
                for (;;) {
                    long n = uaos_read(fd, buf + total_read, 65536 - total_read);
                    if (n <= 0) break;
                    total_read += n;
                    if (total_read >= 65536) break;
                }
                total = total_read;
                long start = total - opt_count;
                if (start < 0) start = 0;
                uaos_write(1, buf + start, total - start);
                return;
            }
            long start = total - opt_count;
            if (start < 0) start = 0;
            while (pos < (uint32_t)start) {
                uint8_t ch; uaos_read_file(fd, &ch, 1); pos++;
            }
            for (;;) {
                uint8_t ch;
                if (pos >= sz) break;
                uaos_read_file(fd, &ch, 1); pos++;
                put_c((char)ch);
            }
        }
        return;
    }

    /* line mode */
    if (opt_sign) {
        /* from line N onward */
        long skip = opt_count - 1;
        if (skip < 0) skip = 0;
        long line_no = 0;
        while (line_no < skip) {
            uint8_t ch;
            long n;
            if (is_stdin) { n = uaos_read(fd, &ch, 1); }
            else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
            if (n <= 0) break;
            if (ch == '\n') line_no++;
        }
        for (;;) {
            uint8_t ch;
            long n;
            if (is_stdin) { n = uaos_read(fd, &ch, 1); }
            else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
            if (n <= 0) break;
            put_c((char)ch);
        }
    } else {
        /* last N lines — ring buffer approach */
        char (*lines)[UAOS_CMD_LINE_MAX] = (char (*)[UAOS_CMD_LINE_MAX])
            uaos_alloc((long)(opt_count > 0 ? opt_count : 10) * UAOS_CMD_LINE_MAX);
        if (!lines) return;
        int *lens = (int *)uaos_alloc((opt_count > 0 ? opt_count : 10) * sizeof(int));
        if (!lens) return;
        long ring_size = opt_count > 0 ? opt_count : 10;
        long ring_head = 0;
        long ring_count = 0;
        int col = 0;
        for (;;) {
            uint8_t ch;
            long n;
            if (is_stdin) { n = uaos_read(fd, &ch, 1); }
            else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
            if (n <= 0) break;
            if (ch == '\n') {
                long idx = (ring_head + ring_count) % ring_size;
                lines[idx][col] = '\0';
                lens[idx] = col;
                if (ring_count < ring_size) ring_count++;
                else ring_head = (ring_head + 1) % ring_size;
                col = 0;
            } else if (ch != '\r') {
                if (col < UAOS_CMD_LINE_MAX - 1) {
                    long idx = (ring_head + ring_count) % ring_size;
                    lines[idx][col++] = (char)ch;
                }
            }
        }
        /* flush last partial line */
        if (col > 0) {
            long idx = (ring_head + ring_count) % ring_size;
            lines[idx][col] = '\0';
            lens[idx] = col;
            if (ring_count < ring_size) ring_count++;
            else ring_head = (ring_head + 1) % ring_size;
        }
        for (long i = 0; i < ring_count; i++) {
            long idx = (ring_head + i) % ring_size;
            uaos_write(1, lines[idx], lens[idx]);
            put_c('\n');
        }
    }
}

static void tail_file(const char *fname, int multi)
{
    if (opt_verbose || (multi && !opt_quiet)) {
        put_s("==> ");
        put_s(fname);
        put_line(" <==");
    }
    if (fname[0] == '-' && fname[1] == '\0') {
        tail_fd(0, 0, 1);
        return;
    }
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(fname, path, sizeof(path));
    long fd = uaos_open(path, UAOS_O_RDONLY);
    if (fd < 0) {
        put_s("tail: cannot open '");
        put_s(fname);
        put_line("' for reading");
        return;
    }
    struct uaos_stat st;
    uint32_t sz = 0;
    if (uaos_stat(path, &st) == 0) sz = st.size;
    tail_fd((int)fd, sz, 0);
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
        tail_fd(0, 0, 1);
        return 0;
    }
    for (int i = 0; i < nfiles; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (fname) tail_file(fname, nfiles > 1);
    }
    return 0;
}
