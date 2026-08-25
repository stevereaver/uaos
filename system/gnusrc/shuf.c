/* shuf.c — GNU coreutils 'shuf' for UAOS gnu: layer
 *
 * Generate random permutations.
 *   shuf [OPTION]... [FILE]
 *   shuf -e [OPTION]... [ARG]...
 *   shuf -i LO-HI [OPTION]...
 * Options: -i, --input-range, -e, --echo, -n COUNT, --head-count,
 *          -o FILE, --output, -r, --repeat, -z, --zero-terminated
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

#define SHUF_MAX 4096
#define SHUF_LINE_MAX UAOS_CMD_LINE_MAX

static int opt_echo = 0;
static int opt_range = 0;
static int opt_repeat = 0;
static int opt_zero = 0;
static long opt_count = -1;
static const char *opt_output = NULL;
static long range_lo = 0, range_hi = 0;

/* Simple xorshift PRNG */
static uint32_t prng_state = 0x12345678;
static uint32_t prng_next(void)
{
    uint32_t x = prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    prng_state = x;
    return x;
}

static void out_str(long out_fd, const char *s)
{
    if (out_fd >= 0) uaos_write_file((int)out_fd, (const uint8_t *)s, (long)uaos_strlen(s));
    else put_s(s);
}

static void out_sep(long out_fd)
{
    if (opt_zero) { if (out_fd >= 0) uaos_write_file((int)out_fd, (const uint8_t *)"\0", 1); else uaos_write(1, "\0", 1); }
    else { if (out_fd >= 0) uaos_write_file((int)out_fd, (const uint8_t *)"\n", 1); else put_c('\n'); }
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"input-range", 'i', required_argument},
        {"echo",        'e', no_argument},
        {"head-count",  'n', required_argument},
        {"output",      'o', required_argument},
        {"repeat",      'r', no_argument},
        {"zero-terminated",'z', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "i:en:o:rz", long_opts, &li)) != -1) {
        switch (opt) {
            case 'i':
                opt_range = 1;
                if (g_optarg) {
                    const char *p = g_optarg;
                    range_lo = 0;
                    while (*p >= '0' && *p <= '9') { range_lo = range_lo * 10 + (*p - '0'); p++; }
                    if (*p == '-') { p++; range_hi = 0; while (*p >= '0' && *p <= '9') { range_hi = range_hi * 10 + (*p - '0'); p++; } }
                }
                break;
            case 'e': opt_echo = 1; break;
            case 'n': { long v; if (uaos_optarg_long(&v)) opt_count = v; } break;
            case 'o': opt_output = g_optarg; break;
            case 'r': opt_repeat = 1; break;
            case 'z': opt_zero = 1; break;
            default:  return 1;
        }
    }

    long out_fd = -1;
    if (opt_output) {
        char path[UAOS_CMD_PATH_MAX];
        cmd_make_abs(opt_output, path, sizeof(path));
        out_fd = uaos_open(path, UAOS_O_WRONLY | UAOS_O_CREAT | UAOS_O_TRUNC);
        if (out_fd < 0) { put_s("shuf: cannot open: "); put_line(opt_output); return 1; }
    }

    if (opt_range) {
        long total = range_hi - range_lo + 1;
        if (opt_repeat) {
            long n = (opt_count >= 0) ? opt_count : total;
            for (long i = 0; i < n; i++) {
                long v = range_lo + (long)(prng_next() % (uint32_t)total);
                char buf[16]; int_to_dec(v, buf, sizeof(buf));
                out_str(out_fd, buf); out_sep(out_fd);
            }
        } else {
            /* Fisher-Yates shuffle of the range */
            long *arr = (long *)uaos_alloc(total * sizeof(long));
            if (!arr) { put_line("shuf: out of memory"); return 1; }
            for (long i = 0; i < total; i++) arr[i] = range_lo + i;
            for (long i = total - 1; i > 0; i--) {
                long j = (long)(prng_next() % (uint32_t)(i + 1));
                long tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
            }
            long n = (opt_count >= 0) ? opt_count : total;
            if (n > total) n = total;
            for (long i = 0; i < n; i++) {
                char buf[16]; int_to_dec(arr[i], buf, sizeof(buf));
                out_str(out_fd, buf); out_sep(out_fd);
            }
        }
        if (out_fd >= 0) uaos_close((int)out_fd);
        return 0;
    }

    if (opt_echo) {
        int nops = uaos_operands_count(argc);
        const char **items = (const char **)uaos_alloc(nops * sizeof(char *));
        if (!items) return 1;
        for (int i = 0; i < nops; i++) items[i] = uaos_operand(argc, argv, i);
        if (opt_repeat) {
            long n = (opt_count >= 0) ? opt_count : nops;
            for (long i = 0; i < n; i++) {
                int idx = (int)(prng_next() % (uint32_t)nops);
                out_str(out_fd, items[idx]); out_sep(out_fd);
            }
        } else {
            for (int i = nops - 1; i > 0; i--) {
                int j = (int)(prng_next() % (uint32_t)(i + 1));
                const char *tmp = items[i]; items[i] = items[j]; items[j] = tmp;
            }
            long n = (opt_count >= 0) ? opt_count : nops;
            if (n > nops) n = nops;
            for (long i = 0; i < n; i++) { out_str(out_fd, items[i]); out_sep(out_fd); }
        }
        if (out_fd >= 0) uaos_close((int)out_fd);
        return 0;
    }

    /* Read lines from file/stdin */
    char (*lines)[SHUF_LINE_MAX] = (char (*)[SHUF_LINE_MAX])
        uaos_alloc((long)(SHUF_MAX * SHUF_LINE_MAX));
    if (!lines) { put_line("shuf: out of memory"); return 1; }
    int count = 0, col = 0;
    int nfiles = uaos_operands_count(argc);
    int fd = 0; uint32_t sz = 0; int is_stdin = 1; uint32_t pos = 0;
    if (nfiles >= 1) {
        const char *fname = uaos_operand(argc, argv, 0);
        if (!(fname[0] == '-' && fname[1] == '\0')) {
            char path[UAOS_CMD_PATH_MAX]; cmd_make_abs(fname, path, sizeof(path));
            fd = (int)uaos_open(path, UAOS_O_RDONLY);
            if (fd < 0) { put_s("shuf: "); put_s(fname); put_line(": No such file"); return 1; }
            struct uaos_stat st; if (uaos_stat(path, &st) == 0) sz = st.size;
            is_stdin = 0;
        }
    }
    char sep = opt_zero ? '\0' : '\n';
    for (;;) {
        uint8_t ch;
        long n;
        if (is_stdin) { n = uaos_read(fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
        if (n <= 0) break;
        if (ch == (uint8_t)sep) {
            if (count < SHUF_MAX) { lines[count][col] = '\0'; count++; }
            col = 0;
        } else if (ch != '\r' || sep != '\n') {
            if (col < SHUF_LINE_MAX - 1) lines[count][col++] = (char)ch;
        }
    }
    if (col > 0 && count < SHUF_MAX) { lines[count][col] = '\0'; count++; }
    if (fd > 0) uaos_close(fd);

    if (opt_repeat) {
        long n = (opt_count >= 0) ? opt_count : count;
        for (long i = 0; i < n; i++) {
            int idx = (int)(prng_next() % (uint32_t)count);
            out_str(out_fd, lines[idx]); out_sep(out_fd);
        }
    } else {
        for (int i = count - 1; i > 0; i--) {
            int j = (int)(prng_next() % (uint32_t)(i + 1));
            char tmp[SHUF_LINE_MAX];
            uaos_strcpy(tmp, lines[i]); uaos_strcpy(lines[i], lines[j]); uaos_strcpy(lines[j], tmp);
        }
        long n = (opt_count >= 0) ? opt_count : count;
        if (n > count) n = count;
        for (long i = 0; i < n; i++) { out_str(out_fd, lines[i]); out_sep(out_fd); }
    }
    if (out_fd >= 0) uaos_close((int)out_fd);
    return 0;
}
