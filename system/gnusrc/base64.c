/* base64.c — GNU coreutils 'base64' for UAOS gnu: layer
 *
 * Encode/decode base64.
 *   base64 [OPTION]... [FILE]
 *   base64 --decode [OPTION]... [FILE]
 * Options: -d, --decode, -w N, --wrap=N, -i, --ignore-garbage
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static const char b64_enc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int opt_decode = 0;
static int opt_wrap = 76;
static int opt_ignore = 0;

static int b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void encode_fd(int fd, uint32_t sz, int is_stdin)
{
    uint8_t in[3];
    int col = 0;
    uint32_t pos = 0;
    for (;;) {
        int n = 0;
        for (int i = 0; i < 3; i++) {
            uint8_t ch;
            long r;
            if (is_stdin) { r = uaos_read(fd, &ch, 1); }
            else { if (pos >= sz) { r = 0; } else { r = uaos_read_file(fd, &ch, 1); pos++; } }
            if (r <= 0) break;
            in[i] = ch; n++;
        }
        if (n == 0) break;
        char out[4];
        out[0] = b64_enc[in[0] >> 2];
        out[1] = b64_enc[((in[0] & 3) << 4) | (n > 1 ? (in[1] >> 4) : 0)];
        out[2] = (n > 1) ? b64_enc[((in[1] & 0xF) << 2) | (n > 2 ? (in[2] >> 6) : 0)] : '=';
        out[3] = (n > 2) ? b64_enc[in[2] & 0x3F] : '=';
        for (int i = 0; i < 4; i++) {
            put_c(out[i]);
            col++;
            if (opt_wrap > 0 && col >= opt_wrap) { put_c('\n'); col = 0; }
        }
    }
    if (col > 0) put_c('\n');
}

static void decode_fd(int fd, uint32_t sz, int is_stdin)
{
    int quad[4];
    int qpos = 0;
    uint32_t pos = 0;
    for (;;) {
        uint8_t ch;
        long n;
        if (is_stdin) { n = uaos_read(fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
        if (n <= 0) break;
        if (ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') continue;
        if (ch == '=') {
            quad[qpos++] = -2;
        } else {
            int v = b64_val(ch);
            if (v < 0) {
                if (opt_ignore) continue;
                put_line("base64: invalid input");
                return;
            }
            quad[qpos++] = v;
        }
        if (qpos == 4) {
            if (quad[0] >= 0) put_c((char)((quad[0] << 2) | (quad[1] >> 4)));
            if (quad[2] != -2 && quad[1] >= 0) put_c((char)((quad[1] << 4) | (quad[2] >> 2)));
            if (quad[3] != -2 && quad[2] >= 0) put_c((char)((quad[2] << 6) | quad[3]));
            qpos = 0;
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
        if (fd < 0) { put_s("base64: "); put_s(fname); put_line(": No such file"); return 1; }
        struct uaos_stat st; if (uaos_stat(path, &st) == 0) sz = st.size;
    }
    if (opt_decode) decode_fd((int)fd, sz, is_stdin);
    else encode_fd((int)fd, sz, is_stdin);
    if (fd > 0) uaos_close((int)fd);
    return 0;
}
