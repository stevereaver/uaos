/* od.c — GNU coreutils 'od' for UAOS gnu: layer
 *
 * Dump files in octal and other formats.
 *   od [OPTION]... [FILE]...
 * Options: -A (d/o/x/n), -t (o/d/x/u/c/a), -j N, -N N, -v
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static char opt_addr_base = 'o';  /* d/o/x/n */
static char opt_format = 'o';     /* o/d/x/u/c/a */
static long opt_skip = 0;
static long opt_read = 0;
static int  opt_all = 0;

static void print_addr(uint32_t addr)
{
    if (opt_addr_base == 'n') return;
    char buf[16];
    if (opt_addr_base == 'd') { uint_to_dec(addr, buf, sizeof(buf)); }
    else if (opt_addr_base == 'x') {
        static const char *h = "0123456789abcdef";
        buf[0] = h[(addr >> 28) & 0xF]; buf[1] = h[(addr >> 24) & 0xF];
        buf[2] = h[(addr >> 20) & 0xF]; buf[3] = h[(addr >> 16) & 0xF];
        buf[4] = h[(addr >> 12) & 0xF]; buf[5] = h[(addr >> 8) & 0xF];
        buf[6] = h[(addr >> 4) & 0xF]; buf[7] = h[addr & 0xF];
        buf[8] = '\0';
    } else {
        /* octal */
        static const char *o = "01234567";
        for (int i = 6; i >= 0; i--) { buf[i] = o[addr & 7]; addr >>= 3; }
        buf[7] = '\0';
    }
    put_s(buf);
    put_c(' ');
}

static void od_fd(int fd, uint32_t sz, int is_stdin)
{
    uint8_t buf[16];
    uint32_t addr = 0;
    uint32_t pos = 0;
    int prev_printed = 0;
    uint8_t prev_buf[16];
    int prev_n = -1;
    int starred = 0;

    /* skip bytes */
    while (opt_skip > 0 && pos < sz) {
        uint8_t ch;
        if (is_stdin) uaos_read(fd, &ch, 1);
        else { uaos_read_file(fd, &ch, 1); }
        pos++; opt_skip--; addr++;
    }

    for (;;) {
        int n = 0;
        for (int i = 0; i < 16; i++) {
            if (opt_read > 0 && addr + n >= (uint32_t)opt_read) break;
            uint8_t ch; long r;
            if (is_stdin) { r = uaos_read(fd, &ch, 1); }
            else { if (pos >= sz) { r = 0; } else { r = uaos_read_file(fd, &ch, 1); pos++; } }
            if (r <= 0) break;
            buf[i] = ch; n++;
        }
        if (n == 0) break;

        /* check for repeated lines */
        if (!opt_all && prev_n == n && n == 16) {
            int same = 1;
            for (int i = 0; i < n; i++) if (buf[i] != prev_buf[i]) { same = 0; break; }
            if (same) {
                if (!starred) { put_line("*"); starred = 1; }
                addr += n;
                continue;
            }
        }
        starred = 0;
        uaos_memcpy(prev_buf, buf, n);
        prev_n = n;

        print_addr(addr);
        for (int i = 0; i < 16; i++) {
            if (i == 8) put_c(' ');
            if (i >= n) {
                put_s("   ");
                continue;
            }
            if (opt_format == 'o') {
                static const char *o = "01234567";
                put_c(o[(buf[i] >> 6) & 3]);
                put_c(o[(buf[i] >> 3) & 7]);
                put_c(o[buf[i] & 7]);
            } else if (opt_format == 'x') {
                static const char *h = "0123456789abcdef";
                put_c(h[buf[i] >> 4]);
                put_c(h[buf[i] & 0xF]);
                put_c(' ');
            } else if (opt_format == 'd' || opt_format == 'u') {
                char num[4];
                int_to_dec(buf[i], num, sizeof(num));
                int slen = (int)uaos_strlen(num);
                for (int p = slen; p < 3; p++) put_c(' ');
                put_s(num);
            } else if (opt_format == 'c') {
                if (buf[i] >= 32 && buf[i] < 127) {
                    put_c(' '); put_c((char)buf[i]); put_c(' ');
                } else {
                    put_s(" \\");
                    if (buf[i] == '\n') put_c('n');
                    else if (buf[i] == '\t') put_c('t');
                    else if (buf[i] == '\r') put_c('r');
                    else if (buf[i] == '\0') put_c('0');
                    else {
                        static const char *o = "01234567";
                        put_c(o[(buf[i] >> 6) & 3]);
                        put_c(o[(buf[i] >> 3) & 7]);
                        put_c(o[buf[i] & 7]);
                    }
                    put_c(' ');
                }
            } else { /* 'a' */
                static const char *names[] = {"nul","soh","stx","etx","eot","enq","ack","bel",
                    " bs"," ht"," nl"," vt"," ff"," cr"," so"," si","dle","dc1","dc2","dc3",
                    "dc4","nak","syn","etb","can"," em","sub","esc"," fs"," gs"," rs"," us"};
                if (buf[i] < 32) put_s(names[buf[i]]);
                else if (buf[i] == 127) put_s("del");
                else { put_c(' '); put_c((char)buf[i]); put_c(' '); }
            }
        }
        put_c('\n');
        addr += n;
        prev_printed = 1;
    }
    if (prev_printed) {
        print_addr(addr);
        put_c('\n');
    }
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"address-base", 'A', required_argument},
        {"format",       't', required_argument},
        {"skip-bytes",   'j', required_argument},
        {"read-bytes",   'N', required_argument},
        {"output-duplicates", 'v', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "A:t:j:N:v", long_opts, &li)) != -1) {
        switch (opt) {
            case 'A': if (g_optarg) opt_addr_base = g_optarg[0]; break;
            case 't': if (g_optarg) {
                for (int i = 0; g_optarg[i]; i++) {
                    if (g_optarg[i] == 'o' || g_optarg[i] == 'd' || g_optarg[i] == 'x' ||
                        g_optarg[i] == 'u' || g_optarg[i] == 'c' || g_optarg[i] == 'a') {
                        opt_format = g_optarg[i]; break;
                    }
                }
            } break;
            case 'j': { long v; if (uaos_optarg_long(&v)) opt_skip = v; } break;
            case 'N': { long v; if (uaos_optarg_long(&v)) opt_read = v; } break;
            case 'v': opt_all = 1; break;
            default:  return 1;
        }
    }
    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) { od_fd(0, 0, 1); return 0; }
    for (int i = 0; i < nfiles; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (!fname) continue;
        if (fname[0] == '-' && fname[1] == '\0') { od_fd(0, 0, 1); continue; }
        char path[UAOS_CMD_PATH_MAX]; cmd_make_abs(fname, path, sizeof(path));
        long fd = uaos_open(path, UAOS_O_RDONLY);
        if (fd < 0) { put_s("od: "); put_s(fname); put_line(": No such file"); continue; }
        struct uaos_stat st; uint32_t sz = 0;
        if (uaos_stat(path, &st) == 0) sz = st.size;
        od_fd((int)fd, sz, 0);
        uaos_close((int)fd);
    }
    return 0;
}
