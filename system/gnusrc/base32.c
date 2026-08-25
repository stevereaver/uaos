/* base32.c — GNU coreutils 'base32' for UAOS gnu: layer
 *
 * Encode/decode base32 (RFC 4648).
 *   base32 [OPTION]... [FILE]
 * Options: -d, --decode, -w N, --wrap=N, -i, --ignore-garbage
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static const char b32_enc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
static int opt_decode = 0;
static int opt_wrap = 76;
static int opt_ignore = 0;

static int b32_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '2' && c <= '7') return c - '2' + 26;
    return -1;
}

static void encode_fd(int fd, uint32_t sz, int is_stdin)
{
    uint8_t in[5];
    int col = 0;
    uint32_t pos = 0;
    for (;;) {
        int n = 0;
        for (int i = 0; i < 5; i++) {
            uint8_t ch; long r;
            if (is_stdin) { r = uaos_read(fd, &ch, 1); }
            else { if (pos >= sz) { r = 0; } else { r = uaos_read_file(fd, &ch, 1); pos++; } }
            if (r <= 0) break;
            in[i] = ch; n++;
        }
        if (n == 0) break;
        char out[8];
        out[0] = b32_enc[in[0] >> 3];
        out[1] = b32_enc[((in[0] & 7) << 2) | (n > 1 ? (in[1] >> 6) : 0)];
        out[2] = (n > 1) ? b32_enc[(in[1] >> 1) & 0x1F] : '=';
        out[3] = (n > 1) ? b32_enc[((in[1] & 1) << 4) | (n > 2 ? (in[2] >> 4) : 0)] : '=';
        out[4] = (n > 2) ? b32_enc[((in[2] & 0xF) << 1) | (n > 3 ? (in[3] >> 7) : 0)] : '=';
        out[5] = (n > 3) ? b32_enc[(in[3] >> 2) & 0x1F] : '=';
        out[6] = (n > 3) ? b32_enc[((in[3] & 3) << 3) | (n > 4 ? (in[4] >> 5) : 0)] : '=';
        out[7] = (n > 4) ? b32_enc[in[4] & 0x1F] : '=';
        for (int i = 0; i < 8; i++) {
            put_c(out[i]); col++;
            if (opt_wrap > 0 && col >= opt_wrap) { put_c('\n'); col = 0; }
        }
    }
    if (col > 0) put_c('\n');
}

static void decode_fd(int fd, uint32_t sz, int is_stdin)
{
    int group[8];
    int gpos = 0;
    uint32_t pos = 0;
    for (;;) {
        uint8_t ch; long n;
        if (is_stdin) { n = uaos_read(fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
        if (n <= 0) break;
        if (ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') continue;
        if (ch == '=') { group[gpos++] = -2; }
        else {
            int v = b32_val(ch);
            if (v < 0) { if (opt_ignore) continue; put_line("base32: invalid input"); return; }
            group[gpos++] = v;
        }
        if (gpos == 8) {
            if (group[0] >= 0 && group[1] >= 0)
                put_c((char)((group[0] << 3) | (group[1] >> 2)));
            if (group[2] >= 0 && group[2] != -2)
                put_c((char)((group[1] << 6) | (group[2] << 1) | (group[3] >> 4)));
            if (group[3] >= 0 && group[4] != -2)
                put_c((char)((group[3] << 4) | (group[4] >> 1)));
            if (group[5] >= 0 && group[5] != -2)
                put_c((char)((group[4] << 7) | (group[5] << 2) | (group[6] >> 3)));
            if (group[6] >= 0 && group[7] != -2)
                put_c((char)((group[6] << 5) | group[7]));
            gpos = 0;
        }
    }
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"decode",         'd', no_argument},
        {"wrap",           'w', required_argument},
        {"ignore-garbage", 'i', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "dw:i", long_opts, &li)) != -1) {
        switch (opt) {
            case 'd': opt_decode = 1; break;
            case 'w': { long v; if (uaos_optarg_long(&v)) opt_wrap = (int)v; } break;
            case 'i': opt_ignore = 1; break;
            default:  return 1;
        }
    }
    int nfiles = uaos_operands_count(argc);
    const char *fname = (nfiles >= 1) ? uaos_operand(argc, argv, 0) : "-";
    int is_stdin = (fname[0] == '-' && fname[1] == '\0');
    long fd; uint32_t sz = 0;
    if (is_stdin) fd = 0;
    else {
        char path[UAOS_CMD_PATH_MAX]; cmd_make_abs(fname, path, sizeof(path));
        fd = uaos_open(path, UAOS_O_RDONLY);
        if (fd < 0) { put_s("base32: "); put_s(fname); put_line(": No such file"); return 1; }
        struct uaos_stat st; if (uaos_stat(path, &st) == 0) sz = st.size;
    }
    if (opt_decode) decode_fd((int)fd, sz, is_stdin);
    else encode_fd((int)fd, sz, is_stdin);
    if (fd > 0) uaos_close((int)fd);
    return 0;
}
