/* basenc.c — GNU coreutils 'basenc' for UAOS gnu: layer
 *
 * Encode/decode various encodings.
 *   basenc --base64 [OPTION]... [FILE]
 * Options: --base16, --base32, --base64, --base64url, -d, --decode, -w N
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static const char b64_enc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char b64url_enc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static const char b32_enc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
static const char *hex_enc = "0123456789abcdef";

static int opt_mode = 0; /* 1=base16, 2=base32, 3=base64, 4=base64url */
static int opt_decode = 0;
static int opt_wrap = 76;

static int enc_val(const char *table, int c)
{
    for (int i = 0; table[i]; i++) if (table[i] == c) return i;
    return -1;
}

static void encode_hex(int fd, uint32_t sz, int is_stdin)
{
    int col = 0;
    uint32_t pos = 0;
    for (;;) {
        uint8_t ch; long n;
        if (is_stdin) { n = uaos_read(fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
        if (n <= 0) break;
        put_c(hex_enc[ch >> 4]); col++;
        put_c(hex_enc[ch & 0xF]); col++;
        if (opt_wrap > 0 && col >= opt_wrap) { put_c('\n'); col = 0; }
    }
    if (col > 0) put_c('\n');
}

static void decode_hex(int fd, uint32_t sz, int is_stdin)
{
    int hi = -1;
    uint32_t pos = 0;
    for (;;) {
        uint8_t ch; long n;
        if (is_stdin) { n = uaos_read(fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
        if (n <= 0) break;
        if (ch == '\n' || ch == ' ') continue;
        int v = -1;
        if (ch >= '0' && ch <= '9') v = ch - '0';
        else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else { put_c((char)((hi << 4) | v)); hi = -1; }
    }
}

static void encode_b64x(int fd, uint32_t sz, int is_stdin, const char *tbl)
{
    uint8_t in[3]; int col = 0; uint32_t pos = 0;
    for (;;) {
        int n = 0;
        for (int i = 0; i < 3; i++) {
            uint8_t ch; long r;
            if (is_stdin) { r = uaos_read(fd, &ch, 1); }
            else { if (pos >= sz) { r = 0; } else { r = uaos_read_file(fd, &ch, 1); pos++; } }
            if (r <= 0) break;
            in[i] = ch; n++;
        }
        if (n == 0) break;
        put_c(tbl[in[0] >> 2]); col++;
        put_c(tbl[((in[0] & 3) << 4) | (n > 1 ? (in[1] >> 4) : 0)]); col++;
        put_c((n > 1) ? tbl[((in[1] & 0xF) << 2) | (n > 2 ? (in[2] >> 6) : 0)] : '='); col++;
        put_c((n > 2) ? tbl[in[2] & 0x3F] : '='); col++;
        if (opt_wrap > 0 && col >= opt_wrap) { put_c('\n'); col = 0; }
    }
    if (col > 0) put_c('\n');
}

static void decode_b64x(int fd, uint32_t sz, int is_stdin, const char *tbl)
{
    int quad[4]; int qpos = 0; uint32_t pos = 0;
    for (;;) {
        uint8_t ch; long n;
        if (is_stdin) { n = uaos_read(fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
        if (n <= 0) break;
        if (ch == '\n' || ch == '\r' || ch == ' ') continue;
        if (ch == '=') { quad[qpos++] = -2; }
        else {
            int v = enc_val(tbl, ch);
            if (v < 0) continue;
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
        {"base16",  1, no_argument},
        {"base32",  2, no_argument},
        {"base64",  3, no_argument},
        {"base64url", 4, no_argument},
        {"decode", 'd', no_argument},
        {"wrap",   'w', required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "dw:", long_opts, &li)) != -1) {
        switch (opt) {
            case 'd': opt_decode = 1; break;
            case 'w': { long v; if (uaos_optarg_long(&v)) opt_wrap = (int)v; } break;
            case UAOS_GO_LONG + 0: opt_mode = 1; break;
            case UAOS_GO_LONG + 1: opt_mode = 2; break;
            case UAOS_GO_LONG + 2: opt_mode = 3; break;
            case UAOS_GO_LONG + 3: opt_mode = 4; break;
            default:  return 1;
        }
    }
    if (opt_mode == 0) { put_line("basenc: must specify --base16/--base32/--base64/--base64url"); return 1; }
    int nfiles = uaos_operands_count(argc);
    const char *fname = (nfiles >= 1) ? uaos_operand(argc, argv, 0) : "-";
    int is_stdin = (fname[0] == '-' && fname[1] == '\0');
    long fd; uint32_t sz = 0;
    if (is_stdin) fd = 0;
    else {
        char path[UAOS_CMD_PATH_MAX]; cmd_make_abs(fname, path, sizeof(path));
        fd = uaos_open(path, UAOS_O_RDONLY);
        if (fd < 0) { put_s("basenc: "); put_s(fname); put_line(": No such file"); return 1; }
        struct uaos_stat st; if (uaos_stat(path, &st) == 0) sz = st.size;
    }
    if (opt_mode == 1) { if (opt_decode) decode_hex((int)fd, sz, is_stdin); else encode_hex((int)fd, sz, is_stdin); }
    else if (opt_mode == 2) { put_line("basenc: base32 not yet supported in this mode"); }
    else if (opt_mode == 3) { if (opt_decode) decode_b64x((int)fd, sz, is_stdin, b64_enc); else encode_b64x((int)fd, sz, is_stdin, b64_enc); }
    else { if (opt_decode) decode_b64x((int)fd, sz, is_stdin, b64url_enc); else encode_b64x((int)fd, sz, is_stdin, b64url_enc); }
    if (fd > 0) uaos_close((int)fd);
    return 0;
}
