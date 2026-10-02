/* uudecode.c — GNU sharutils 'uudecode' for UAOS gnu: layer
 *
 * Decode a uuencoded (or begin-base64) file back to binary.
 *   uudecode [OPTION]... [FILE]
 * Options: -o FILE, --output-file=FILE   ('-' or /dev/stdout = stdout)
 *
 * Reads stdin when FILE is omitted or '-'.  Text preceding the
 * "begin <mode> <name>" header is skipped; the output file is named
 * by the header unless -o overrides it.  The octal mode is applied
 * back as AmigaDOS protection bits (owner rwx -> FIBF read/write/
 * execute deny bits, the inverse of gnu chmod).
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static const char *opt_output = NULL;

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

/* Read one line (CR/LF terminated) into buf, NUL-terminated with the
 * terminator stripped.  Returns line length, or -1 at EOF. */
static int in_line(UuIn *in, char *buf, int max)
{
    int n = 0;
    for (;;) {
        int c = in_getc(in);
        if (c < 0) {
            if (n == 0) return -1;
            break;
        }
        if (c == '\n') break;
        if (n < max - 1) buf[n++] = (char)c;
    }
    if (n > 0 && buf[n - 1] == '\r') n--;
    buf[n] = '\0';
    return n;
}

/* -------------------------------------------------------------------------
 * Buffered output sink — stdout or a real file
 * ------------------------------------------------------------------------- */
typedef struct {
    int     fd;
    int     is_stdout;
    int     err;
    int     len;
    uint8_t buf[256];
} UuOut;

static void out_flush(UuOut *o)
{
    if (o->len == 0 || o->err) return;
    if (o->is_stdout) uaos_stdout_write(o->buf, o->len);
    else if (uaos_write_file(o->fd, o->buf, o->len) < o->len) o->err = 1;
    o->len = 0;
}

static void out_put(UuOut *o, uint8_t b)
{
    if (o->len >= (int)sizeof(o->buf)) out_flush(o);
    if (!o->err) o->buf[o->len++] = b;
}

/* -------------------------------------------------------------------------
 * Traditional uudecode
 * ------------------------------------------------------------------------- */
static int dec_char(int c)
{
    if (c < ' ') c = ' ';
    return (c - ' ') & 077;   /* ' ' and '`' both decode to 0 */
}

/* Decode one data line.  Returns 0 while data was emitted, 1 when the
 * line is a terminator (zero-length: ' ', '`', blank, or junk). */
static int dec_uu_line(const char *line, int ll, UuOut *o)
{
    if (ll < 1) return 1;
    int n = line[0] - ' ';
    if (n <= 0 || n > 45) return 1;
    int emitted = 0;
    for (int i = 1; emitted < n; i += 4) {
        int c0 = (i     < ll) ? dec_char(line[i])     : 0;
        int c1 = (i + 1 < ll) ? dec_char(line[i + 1]) : 0;
        int c2 = (i + 2 < ll) ? dec_char(line[i + 2]) : 0;
        int c3 = (i + 3 < ll) ? dec_char(line[i + 3]) : 0;
        out_put(o, (uint8_t)((c0 << 2) | (c1 >> 4))); emitted++;
        if (emitted >= n) break;
        out_put(o, (uint8_t)((c1 << 4) | (c2 >> 2))); emitted++;
        if (emitted >= n) break;
        out_put(o, (uint8_t)((c2 << 6) | c3));        emitted++;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * begin-base64 body (same quad decoder as gnu base64 -d, garbage skipped)
 * ------------------------------------------------------------------------- */
static int b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void dec_b64_line(const char *line, int ll, UuOut *o)
{
    int quad[4];
    int q = 0;
    for (int i = 0; i < ll; i++) {
        char ch = line[i];
        int v;
        if (ch == '=') v = -2;
        else { v = b64_val(ch); if (v < 0) continue; }
        quad[q++] = v;
        if (q == 4) {
            if (quad[0] >= 0) out_put(o, (uint8_t)((quad[0] << 2) | (quad[1] >> 4)));
            if (quad[2] != -2 && quad[1] >= 0) out_put(o, (uint8_t)((quad[1] << 4) | (quad[2] >> 2)));
            if (quad[3] != -2 && quad[2] >= 0) out_put(o, (uint8_t)((quad[2] << 6) | quad[3]));
            q = 0;
        }
    }
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"output-file", 'o', required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "o:", long_opts, &li)) != -1) {
        switch (opt) {
            case 'o': opt_output = g_optarg; break;
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    if (nops > 1) {
        put_line("usage: uudecode [-o outfile] [infile]");
        return 1;
    }
    const char *fname = (nops == 1) ? uaos_operand(argc, argv, 0) : "-";

    UuIn in;
    in.size = 0;
    in.pos = 0;
    in.is_stdin = (fname[0] == '-' && fname[1] == '\0');
    if (in.is_stdin) {
        in.fd = 0;
    } else {
        char path[UAOS_CMD_PATH_MAX];
        cmd_make_abs(fname, path, sizeof(path));
        long fd = uaos_open(path, UAOS_O_RDONLY);
        if (fd < 0) {
            put_s("uudecode: "); put_s(fname); put_line(": No such file");
            return 1;
        }
        struct uaos_stat st;
        if (uaos_stat(path, &st) == 0) in.size = st.size;
        in.fd = (int)fd;
    }

    /* Scan for the begin / begin-base64 header. */
    char line[1024];
    char name[UAOS_CMD_PATH_MAX];
    int is_b64 = 0;
    int found = 0;
    long mode = 0644;
    while (in_line(&in, line, sizeof(line)) >= 0) {
        const char *p;
        if (uaos_strncmp(line, "begin-base64", 12) == 0) {
            is_b64 = 1; p = line + 12;
        } else if (uaos_strncmp(line, "begin", 5) == 0 &&
                   (line[5] == ' ' || line[5] == '\t' || line[5] == '\0')) {
            is_b64 = 0; p = line + 5;
        } else continue;

        while (*p == ' ' || *p == '\t') p++;
        long m = 0;
        int has_mode = 0;
        while (*p >= '0' && *p <= '7') { m = m * 8 + (*p - '0'); has_mode = 1; p++; }
        if (has_mode) {
            mode = m & 0777;
            while (*p == ' ' || *p == '\t') p++;
        }
        if (!*p) { put_line("uudecode: no filename in begin line"); return 1; }
        uaos_strncpy(name, p, sizeof(name));
        name[sizeof(name) - 1] = '\0';
        int nl = (int)uaos_strlen(name);
        while (nl > 0 && (name[nl - 1] == ' ' || name[nl - 1] == '\t'))
            name[--nl] = '\0';
        found = 1;
        break;
    }
    if (!found) {
        put_line("uudecode: no begin line found");
        if (!in.is_stdin) uaos_close(in.fd);
        return 1;
    }

    /* Resolve the destination. */
    const char *dest = opt_output ? opt_output : name;
    UuOut out;
    out.err = 0;
    out.len = 0;
    char opath[UAOS_CMD_PATH_MAX];
    if ((dest[0] == '-' && dest[1] == '\0') ||
        uaos_strcmp(dest, "/dev/stdout") == 0) {
        out.is_stdout = 1;
        out.fd = 1;
    } else {
        out.is_stdout = 0;
        cmd_make_abs(dest, opath, sizeof(opath));
        long fd = uaos_open(opath, UAOS_O_WRONLY | UAOS_O_CREAT | UAOS_O_TRUNC);
        if (fd < 0) {
            put_s("uudecode: "); put_s(dest); put_line(": cannot create");
            if (!in.is_stdin) uaos_close(in.fd);
            return 1;
        }
        out.fd = (int)fd;
    }

    /* Decode the body. */
    if (is_b64) {
        int ll;
        while ((ll = in_line(&in, line, sizeof(line))) >= 0) {
            if (uaos_strncmp(line, "====", 4) == 0) break;
            dec_b64_line(line, ll, &out);
        }
    } else {
        int ll;
        while ((ll = in_line(&in, line, sizeof(line))) >= 0) {
            if (dec_uu_line(line, ll, &out)) break;
        }
    }
    out_flush(&out);

    if (!in.is_stdin) uaos_close(in.fd);
    if (out.err) {
        put_s("uudecode: "); put_s(dest); put_line(": write error");
        return 1;
    }
    if (!out.is_stdout) {
        uaos_close(out.fd);
        /* Apply the header mode as AmigaDOS protection bits (owner rwx). */
        uint16_t prot = 0;
        if (!(mode & 0400)) prot |= UAOS_FIBF_READ;
        if (!(mode & 0200)) prot |= UAOS_FIBF_WRITE;
        if (!(mode & 0100)) prot |= UAOS_FIBF_EXECUTE;
        uaos_setprotection(opath, prot);
    }
    return 0;
}
