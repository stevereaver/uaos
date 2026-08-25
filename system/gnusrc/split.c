/* split.c — GNU coreutils 'split' for UAOS gnu: layer
 *
 * Split a file into pieces.
 *   split [OPTION]... [FILE [PREFIX]]
 * Options: -l N, --lines=N, -b N, --bytes=N, -a N, --suffix-length=N,
 *          -d, --numeric-suffixes, --additional-suffix=STR
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static long opt_lines = 1000;
static long opt_bytes = 0;
static int  opt_suffix_len = 2;
static int  opt_numeric = 0;
static char opt_add_suffix[16] = "";

static void make_suffix(int idx, char *out)
{
    if (opt_numeric) {
        char buf[12];
        int_to_dec(idx, buf, sizeof(buf));
        int slen = (int)uaos_strlen(buf);
        int pad = opt_suffix_len - slen;
        for (int i = 0; i < pad; i++) out[i] = '0';
        uaos_strcpy(out + (pad > 0 ? pad : 0), buf);
    } else {
        /* alphabetic suffix: aa, ab, ... zz, aaa, ... */
        for (int i = 0; i < opt_suffix_len; i++) out[i] = 'a';
        out[opt_suffix_len] = '\0';
        for (int pos = opt_suffix_len - 1; idx > 0 && pos >= 0; pos--) {
            out[pos] = (char)('a' + (idx % 26));
            idx /= 26;
        }
    }
}

static long open_split(const char *prefix, int idx)
{
    char name[UAOS_CMD_PATH_MAX];
    char suffix[16];
    make_suffix(idx, suffix);
    /* build: prefix + suffix + additional_suffix */
    int i = 0;
    while (prefix[i] && i < (int)sizeof(name) - 1) { name[i] = prefix[i]; i++; }
    int j = 0;
    while (suffix[j] && i < (int)sizeof(name) - 1) { name[i++] = suffix[j++]; }
    j = 0;
    while (opt_add_suffix[j] && i < (int)sizeof(name) - 1) { name[i++] = opt_add_suffix[j++]; }
    name[i] = '\0';

    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(name, path, sizeof(path));
    long fd = uaos_open(path, UAOS_O_WRONLY | UAOS_O_CREAT | UAOS_O_TRUNC);
    if (fd < 0) { put_s("split: cannot create: "); put_line(name); }
    return fd;
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"lines",     'l', required_argument},
        {"bytes",     'b', required_argument},
        {"suffix-length", 'a', required_argument},
        {"numeric-suffixes", 'd', no_argument},
        {"additional-suffix", 0, required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "l:b:a:d", long_opts, &li)) != -1) {
        switch (opt) {
            case 'l': { long v; if (uaos_optarg_long(&v)) opt_lines = v; opt_bytes = 0; } break;
            case 'b': { long v; if (uaos_optarg_long(&v)) opt_bytes = v; opt_lines = 0; } break;
            case 'a': { long v; if (uaos_optarg_long(&v) && v > 0) opt_suffix_len = (int)v; } break;
            case 'd': opt_numeric = 1; break;
            case UAOS_GO_LONG + 4:
                if (g_optarg) { int i=0; while (g_optarg[i] && i < (int)sizeof(opt_add_suffix)-1) { opt_add_suffix[i]=g_optarg[i]; i++; } opt_add_suffix[i]='\0'; }
                break;
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    const char *fname = (nops >= 1) ? uaos_operand(argc, argv, 0) : "-";
    const char *prefix = (nops >= 2) ? uaos_operand(argc, argv, 1) : "x";

    int is_stdin = (fname[0] == '-' && fname[1] == '\0');
    long fd;
    uint32_t sz = 0, pos = 0;
    if (is_stdin) fd = 0;
    else {
        char path[UAOS_CMD_PATH_MAX]; cmd_make_abs(fname, path, sizeof(path));
        fd = uaos_open(path, UAOS_O_RDONLY);
        if (fd < 0) { put_s("split: "); put_s(fname); put_line(": No such file"); return 1; }
        struct uaos_stat st; if (uaos_stat(path, &st) == 0) sz = st.size;
    }

    int file_idx = 0;
    long out_fd = open_split(prefix, file_idx);
    if (out_fd < 0) return 1;

    if (opt_bytes > 0) {
        long bytes_in_file = 0;
        for (;;) {
            uint8_t ch;
            long n;
            if (is_stdin) { n = uaos_read(fd, &ch, 1); }
            else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
            if (n <= 0) break;
            uaos_write_file((int)out_fd, &ch, 1);
            bytes_in_file++;
            if (bytes_in_file >= opt_bytes) {
                uaos_close((int)out_fd);
                file_idx++;
                out_fd = open_split(prefix, file_idx);
                if (out_fd < 0) return 1;
                bytes_in_file = 0;
            }
        }
    } else {
        long lines_in_file = 0;
        for (;;) {
            uint8_t ch;
            long n;
            if (is_stdin) { n = uaos_read(fd, &ch, 1); }
            else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
            if (n <= 0) break;
            uaos_write_file((int)out_fd, &ch, 1);
            if (ch == '\n') {
                lines_in_file++;
                if (lines_in_file >= opt_lines) {
                    uaos_close((int)out_fd);
                    file_idx++;
                    out_fd = open_split(prefix, file_idx);
                    if (out_fd < 0) return 1;
                    lines_in_file = 0;
                }
            }
        }
    }
    if (out_fd >= 0) uaos_close((int)out_fd);
    if (fd > 0) uaos_close((int)fd);
    return 0;
}
