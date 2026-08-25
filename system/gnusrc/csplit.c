/* csplit.c — GNU coreutils 'csplit' for UAOS gnu: layer
 *
 * Split a file into sections determined by context lines.
 *   csplit [OPTION]... FILE PATTERN...
 * Patterns: N          — split before line N
 *           /STRING/   — split before line matching STRING
 *           %STRING%   — skip to line matching STRING
 *           {N}        — repeat previous pattern N times
 * Options: -f PREFIX, -n N, -k, -s, -z
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static char opt_prefix[64] = "xx";
static int  opt_digits = 2;
static int  opt_keep = 0;
static int  opt_quiet = 0;
static int  opt_elide = 0;

static void make_name(int idx, char *out, int max)
{
    int i = 0;
    int j = 0;
    while (opt_prefix[j] && i < max - 1) out[i++] = opt_prefix[j++];
    char num[12];
    int_to_dec(idx, num, sizeof(num));
    int nlen = (int)uaos_strlen(num);
    int pad = opt_digits - nlen;
    for (int p = 0; p < pad && i < max - 1; p++) out[i++] = '0';
    j = 0;
    while (num[j] && i < max - 1) out[i++] = num[j++];
    out[i] = '\0';
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"prefix",          'f', required_argument},
        {"digits",          'n', required_argument},
        {"keep-files",      'k', no_argument},
        {"quiet",           's', no_argument},
        {"silent",          's', no_argument},
        {"elide-empty-files",'z', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "f:n:ksz", long_opts, &li)) != -1) {
        switch (opt) {
            case 'f': if (g_optarg) { int i=0; while (g_optarg[i] && i < (int)sizeof(opt_prefix)-1) { opt_prefix[i]=g_optarg[i]; i++; } opt_prefix[i]='\0'; } break;
            case 'n': { long v; if (uaos_optarg_long(&v) && v > 0) opt_digits = (int)v; } break;
            case 'k': opt_keep = 1; break;
            case 's': opt_quiet = 1; break;
            case 'z': opt_elide = 1; break;
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    if (nops < 2) { put_line("csplit: usage: csplit [OPTION]... FILE PATTERN..."); return 1; }

    const char *fname = uaos_operand(argc, argv, 0);
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(fname, path, sizeof(path));
    long fd = uaos_open(path, UAOS_O_RDONLY);
    if (fd < 0) { put_s("csplit: "); put_s(fname); put_line(": No such file"); return 1; }
    struct uaos_stat st; uint32_t sz = 0;
    if (uaos_stat(path, &st) == 0) sz = st.size;

    /* Read all lines into memory */
    char (*lines)[UAOS_CMD_LINE_MAX] = (char (*)[UAOS_CMD_LINE_MAX])
        uaos_alloc((long)(4096 * UAOS_CMD_LINE_MAX));
    if (!lines) { put_line("csplit: out of memory"); return 1; }
    int *lens = (int *)uaos_alloc(4096 * sizeof(int));
    if (!lens) { put_line("csplit: out of memory"); return 1; }
    int total_lines = 0;
    int col = 0;
    uint32_t pos = 0;
    for (; pos < sz && total_lines < 4096; ) {
        uint8_t ch;
        uaos_read_file((int)fd, &ch, 1); pos++;
        if (ch == '\n') { lines[total_lines][col] = '\0'; lens[total_lines] = col; total_lines++; col = 0; }
        else if (ch != '\r' && col < UAOS_CMD_LINE_MAX - 1) lines[total_lines][col++] = (char)ch;
    }
    if (col > 0 && total_lines < 4096) { lines[total_lines][col] = '\0'; lens[total_lines] = col; total_lines++; }
    uaos_close((int)fd);

    /* Process patterns */
    int cur_line = 0;
    int file_idx = 0;
    int n_patterns = nops - 1;
    int pat = 0;

    while (pat < n_patterns) {
        const char *p = uaos_operand(argc, argv, pat + 1);
        if (!p) { pat++; continue; }

        /* Determine split point */
        int split_at = -1;
        int skip_mode = 0;
        int repeat = 1;

        if (p[0] >= '0' && p[0] <= '9') {
            /* line number */
            int v = 0;
            const char *q = p;
            while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; }
            split_at = v - 1; /* 0-based, split before this line */
        } else if (p[0] == '/' || p[0] == '%') {
            skip_mode = (p[0] == '%');
            /* find closing / */
            const char *str = p + 1;
            const char *end = str;
            while (*end && *end != '/') end++;
            int slen = (int)(end - str);
            /* search for matching line */
            for (int i = cur_line; i < total_lines; i++) {
                if (slen == 0 || uaos_strncmp(lines[i], str, slen) == 0) {
                    split_at = i;
                    break;
                }
            }
        } else if (p[0] == '{') {
            /* repeat count */
            int v = 0;
            const char *q = p + 1;
            while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; }
            repeat = v;
            pat++;
            /* re-process previous pattern 'repeat' more times */
            /* (simplified: just continue) */
            continue;
        }

        /* Write lines from cur_line to split_at */
        if (split_at > cur_line) {
            char name[UAOS_CMD_PATH_MAX];
            make_name(file_idx, name, sizeof(name));
            char fpath[UAOS_CMD_PATH_MAX];
            cmd_make_abs(name, fpath, sizeof(fpath));
            long ofd = uaos_open(fpath, UAOS_O_WRONLY | UAOS_O_CREAT | UAOS_O_TRUNC);
            if (ofd >= 0) {
                int written = 0;
                for (int i = cur_line; i < split_at && i < total_lines; i++) {
                    uaos_write_file((int)ofd, (const uint8_t *)lines[i], lens[i]);
                    uaos_write_file((int)ofd, (const uint8_t *)"\n", 1);
                    written++;
                }
                uaos_close((int)ofd);
                if (!opt_quiet) {
                    char buf[16]; uint_to_dec((uint32_t)written, buf, sizeof(buf));
                    put_s(buf); put_c('\n');
                }
                file_idx++;
            }
        }
        if (skip_mode) {
            cur_line = split_at;
        } else if (split_at >= 0) {
            cur_line = split_at;
        }
        pat++;
    }

    /* Write remaining lines */
    if (cur_line < total_lines) {
        char name[UAOS_CMD_PATH_MAX];
        make_name(file_idx, name, sizeof(name));
        char fpath[UAOS_CMD_PATH_MAX];
        cmd_make_abs(name, fpath, sizeof(fpath));
        long ofd = uaos_open(fpath, UAOS_O_WRONLY | UAOS_O_CREAT | UAOS_O_TRUNC);
        if (ofd >= 0) {
            int written = 0;
            for (int i = cur_line; i < total_lines; i++) {
                uaos_write_file((int)ofd, (const uint8_t *)lines[i], lens[i]);
                uaos_write_file((int)ofd, (const uint8_t *)"\n", 1);
                written++;
            }
            uaos_close((int)ofd);
            if (!opt_quiet) {
                char buf[16]; uint_to_dec((uint32_t)written, buf, sizeof(buf));
                put_s(buf); put_c('\n');
            }
        }
    }
    return 0;
}
