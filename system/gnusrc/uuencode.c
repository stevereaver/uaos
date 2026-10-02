/* uuencode.c — GNU sharutils 'uuencode' for UAOS gnu: layer
 *
 * Encode a binary file into an ASCII mail-safe representation.
 *   uuencode [OPTION]... [INFILE] REMOTEFILE
 * Options: -m, --base64
 *
 * Traditional output: "begin <mode> <name>", data lines of up to 45
 * bytes (length char + 6-bit groups biased by ' ', '`' for zero),
 * a "`" terminator line, then "end".
 * -m output: "begin-base64 <mode> <name>", 76-column base64, "====".
 *
 * <mode> is the classic octal permission field.  It is derived from
 * the file's AmigaDOS protection bits using the inverse of the gnu
 * chmod mapping (owner rwx; group/other get read+execute only), so a
 * default file encodes as 755 and chmod 644 round-trips as 644.
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_base64 = 0;

static const char b64_enc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

typedef struct {
    int      fd;
    int      is_stdin;
    uint32_t size;
    uint32_t pos;
} UuIn;

static int in_getc(UuIn *in)
{
    uint8_t ch;
    long n;
    if (in->is_stdin) {
        n = uaos_read(in->fd, &ch, 1);
    } else {
        if (in->pos >= in->size) return -1;
        n = uaos_read_file(in->fd, &ch, 1);
        in->pos++;
    }
    return (n <= 0) ? -1 : (int)ch;
}

/* 6-bit value to uuencode char: zero encodes as '`' (modern form). */
static char enc6(int v) { return v ? (char)(v + ' ') : '`'; }

static void enc_uu_line(const uint8_t *buf, int n)
{
    put_c((char)(n + ' '));
    for (int i = 0; i < n; i += 3) {
        int c0 = buf[i];
        int c1 = (i + 1 < n) ? buf[i + 1] : 0;
        int c2 = (i + 2 < n) ? buf[i + 2] : 0;
        put_c(enc6(c0 >> 2));
        put_c(enc6(((c0 & 3) << 4) | (c1 >> 4)));
        put_c(enc6(((c1 & 15) << 2) | (c2 >> 6)));
        put_c(enc6(c2 & 63));
    }
    put_c('\n');
}

static void encode_uu(UuIn *in)
{
    uint8_t buf[45];
    for (;;) {
        int n = 0;
        while (n < 45) {
            int c = in_getc(in);
            if (c < 0) break;
            buf[n++] = (uint8_t)c;
        }
        if (n == 0) break;
        enc_uu_line(buf, n);
    }
    put_line("`");
    put_line("end");
}

static void encode_b64(UuIn *in)
{
    uint8_t ibuf[3];
    int col = 0;
    for (;;) {
        int n = 0;
        while (n < 3) {
            int c = in_getc(in);
            if (c < 0) break;
            ibuf[n++] = (uint8_t)c;
        }
        if (n == 0) break;
        char out[4];
        out[0] = b64_enc[ibuf[0] >> 2];
        out[1] = b64_enc[((ibuf[0] & 3) << 4) | (n > 1 ? (ibuf[1] >> 4) : 0)];
        out[2] = (n > 1) ? b64_enc[((ibuf[1] & 0xF) << 2) | (n > 2 ? (ibuf[2] >> 6) : 0)] : '=';
        out[3] = (n > 2) ? b64_enc[ibuf[2] & 0x3F] : '=';
        for (int i = 0; i < 4; i++) {
            put_c(out[i]);
            if (++col >= 76) { put_c('\n'); col = 0; }
        }
    }
    if (col > 0) put_c('\n');
    put_line("====");
}

/* AmigaDOS protection bits -> octal mode: owner rwx straight,
 * group/other = owner minus write (r-x style). */
static int mode_from_prot(uint16_t prot)
{
    int u = 0;
    if (!(prot & UAOS_FIBF_READ))    u |= 4;
    if (!(prot & UAOS_FIBF_WRITE))   u |= 2;
    if (!(prot & UAOS_FIBF_EXECUTE)) u |= 1;
    return (u << 6) | ((u & 5) << 3) | (u & 5);
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"base64", 'm', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "m", long_opts, &li)) != -1) {
        switch (opt) {
            case 'm': opt_base64 = 1; break;
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    if (nops < 1 || nops > 2) {
        put_line("usage: uuencode [-m] [infile] remotefile");
        return 1;
    }
    const char *fname = (nops == 2) ? uaos_operand(argc, argv, 0) : "-";
    const char *rname = uaos_operand(argc, argv, nops - 1);

    UuIn in;
    in.size = 0;
    in.pos = 0;
    in.is_stdin = (fname[0] == '-' && fname[1] == '\0');
    int mode = 0644;
    if (in.is_stdin) {
        in.fd = 0;
    } else {
        char path[UAOS_CMD_PATH_MAX];
        cmd_make_abs(fname, path, sizeof(path));
        long fd = uaos_open(path, UAOS_O_RDONLY);
        if (fd < 0) {
            put_s("uuencode: "); put_s(fname); put_line(": No such file");
            return 1;
        }
        struct uaos_stat st;
        if (uaos_stat(path, &st) == 0) {
            in.size = st.size;
            mode = mode_from_prot(st.protection);
        }
        in.fd = (int)fd;
    }

    if (opt_base64) put_s("begin-base64 ");
    else            put_s("begin ");
    put_c((char)('0' + ((mode >> 6) & 7)));
    put_c((char)('0' + ((mode >> 3) & 7)));
    put_c((char)('0' + (mode & 7)));
    put_c(' ');
    put_s(rname);
    put_c('\n');

    if (opt_base64) encode_b64(&in);
    else            encode_uu(&in);

    if (!in.is_stdin) uaos_close(in.fd);
    return 0;
}
