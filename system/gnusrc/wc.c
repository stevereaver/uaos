/* wc.c — GNU coreutils 'wc' for UAOS gnu: layer
 *
 * Print newline, word, and byte counts for each file.
 *   wc [OPTION]... [FILE]...
 * Options: -c, --bytes, -w, --words, -l, --lines, -m, --chars, -L, --max-line-length
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_bytes = 0;
static int opt_words = 0;
static int opt_lines = 0;
static int opt_chars = 0;
static int opt_maxlen = 0;
static int any_opt = 0;

static void print_count(uint32_t val, int width)
{
    char buf[16];
    uint_to_dec(val, buf, sizeof(buf));
    int slen = (int)uaos_strlen(buf);
    if (slen < width) {
        for (int i = 0; i < width - slen; i++) put_c(' ');
    }
    put_s(buf);
}

static void wc_fd(int fd, uint32_t sz, int is_stdin,
                  uint32_t *out_lines, uint32_t *out_words,
                  uint32_t *out_bytes, uint32_t *out_maxlen)
{
    uint32_t lines = 0, words = 0, bytes = 0, maxlen = 0;
    int in_word = 0;
    uint32_t line_len = 0;
    uint32_t pos = 0;

    for (;;) {
        uint8_t ch;
        long n;
        if (is_stdin) { n = uaos_read(fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
        if (n <= 0) break;

        bytes++;
        if (ch == '\n') {
            lines++;
            if (line_len > maxlen) maxlen = line_len;
            line_len = 0;
            in_word = 0;
            continue;
        }
        if (ch == '\r') continue;
        line_len++;

        if (uaos_isspace(ch)) {
            in_word = 0;
        } else {
            if (!in_word) { words++; in_word = 1; }
        }
    }
    if (line_len > maxlen) maxlen = line_len;

    *out_lines = lines;
    *out_words = words;
    *out_bytes = bytes;
    *out_maxlen = maxlen;
}

static int wc_file(const char *fname, int print_name)
{
    uint32_t lines, words, bytes, maxlen;
    int is_stdin = (fname[0] == '-' && fname[1] == '\0');

    if (is_stdin) {
        wc_fd(0, 0, 1, &lines, &words, &bytes, &maxlen);
    } else {
        char path[UAOS_CMD_PATH_MAX];
        cmd_make_abs(fname, path, sizeof(path));
        long fd = uaos_open(path, UAOS_O_RDONLY);
        if (fd < 0) {
            put_s("wc: ");
            put_s(fname);
            put_line(": No such file or directory");
            return 1;
        }
        struct uaos_stat st;
        uint32_t sz = 0;
        if (uaos_stat(path, &st) == 0) sz = st.size;
        wc_fd((int)fd, sz, 0, &lines, &words, &bytes, &maxlen);
        uaos_close((int)fd);
    }

    int w = 7;
    int first = 1;
    if (opt_lines || (!any_opt)) { if (!first) put_c(' '); print_count(lines, w); first = 0; }
    if (opt_words || (!any_opt)) { if (!first) put_c(' '); print_count(words, w); first = 0; }
    if (opt_chars || opt_bytes || (!any_opt)) { if (!first) put_c(' '); print_count(bytes, w); first = 0; }
    if (opt_maxlen) { if (!first) put_c(' '); print_count(maxlen, w); first = 0; }
    if (print_name) { put_c(' '); put_s(fname); }
    put_c('\n');
    return 0;
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"bytes",            'c', no_argument},
        {"words",            'w', no_argument},
        {"lines",            'l', no_argument},
        {"chars",            'm', no_argument},
        {"max-line-length",  'L', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "cwlmL", long_opts, &li)) != -1) {
        any_opt = 1;
        switch (opt) {
            case 'c': opt_bytes = 1; break;
            case 'w': opt_words = 1; break;
            case 'l': opt_lines = 1; break;
            case 'm': opt_chars = 1; break;
            case 'L': opt_maxlen = 1; break;
            default:  return 1;
        }
    }

    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) {
        wc_file("-", 0);
        return 0;
    }
    int rc = 0;
    for (int i = 0; i < nfiles; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (fname) { if (wc_file(fname, nfiles > 1) != 0) rc = 1; }
    }
    return rc;
}
