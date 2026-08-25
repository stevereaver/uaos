/* md5sum.c — GNU coreutils 'md5sum' for UAOS gnu: layer
 *
 * Compute and check MD5 message digests.
 *   md5sum [OPTION]... [FILE]...
 * Options: -c, --check, --tag, -w, --warn, -z, --zero
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"
#include "uaos_hash.h"

static int opt_check = 0;
static int opt_zero = 0;

static void compute_md5(int fd, uint32_t sz, int is_stdin, uint8_t out[16])
{
    md5_ctx ctx;
    md5_init(&ctx);
    uint32_t pos = 0;
    for (;;) {
        uint8_t buf[4096];
        long n = 0;
        if (is_stdin) { n = uaos_read(fd, buf, sizeof(buf)); }
        else {
            long to_read = (long)(sz - pos < sizeof(buf) ? sz - pos : sizeof(buf));
            if (to_read <= 0) break;
            n = uaos_read_file(fd, buf, to_read);
            pos += (uint32_t)n;
        }
        if (n <= 0) break;
        md5_update(&ctx, buf, (size_t)n);
    }
    md5_final(&ctx, out);
}

static void md5sum_file(const char *fname)
{
    int is_stdin = (fname[0] == '-' && fname[1] == '\0');
    long fd; uint32_t sz = 0;
    if (is_stdin) fd = 0;
    else {
        char path[UAOS_CMD_PATH_MAX]; cmd_make_abs(fname, path, sizeof(path));
        fd = uaos_open(path, UAOS_O_RDONLY);
        if (fd < 0) { put_s("md5sum: "); put_s(fname); put_line(": No such file or directory"); return; }
        struct uaos_stat st; if (uaos_stat(path, &st) == 0) sz = st.size;
    }
    uint8_t digest[16];
    compute_md5((int)fd, sz, is_stdin, digest);
    if (fd > 0) uaos_close((int)fd);
    char hex[33];
    hash_to_hex(digest, 16, hex);
    put_s(hex);
    put_s("  ");
    put_s(fname);
    put_c('\n');
}

static void check_file(const char *fname)
{
    int is_stdin = (fname[0] == '-' && fname[1] == '\0');
    long fd; uint32_t sz = 0;
    if (is_stdin) fd = 0;
    else {
        char path[UAOS_CMD_PATH_MAX]; cmd_make_abs(fname, path, sizeof(path));
        fd = uaos_open(path, UAOS_O_RDONLY);
        if (fd < 0) { put_s("md5sum: "); put_s(fname); put_line(": No such file"); return; }
        struct uaos_stat st; if (uaos_stat(path, &st) == 0) sz = st.size;
    }
    char line[UAOS_CMD_LINE_MAX * 2];
    int col = 0;
    uint32_t pos = 0;
    for (;;) {
        uint8_t ch; long n;
        if (is_stdin) { n = uaos_read(fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
        if (n <= 0) break;
        if (ch == '\n') {
            line[col] = '\0';
            /* parse: HEX  FILENAME */
            if (col >= 34) {
                char expected[33];
                uaos_strncpy(expected, line, 32); expected[32] = '\0';
                const char *sep = line + 32;
                while (*sep == ' ' || *sep == '*') sep++;
                /* compute md5 of file */
                long ffd = uaos_open(sep, UAOS_O_RDONLY);
                if (ffd >= 0) {
                    struct uaos_stat st2; uint32_t fsz = 0;
                    if (uaos_stat(sep, &st2) == 0) fsz = st2.size;
                    uint8_t digest[16];
                    compute_md5((int)ffd, fsz, 0, digest);
                    uaos_close((int)ffd);
                    char hex[33];
                    hash_to_hex(digest, 16, hex);
                    if (uaos_strcmp(hex, expected) == 0) {
                        put_s(sep); put_s(": OK");
                    } else {
                        put_s(sep); put_s(": FAILED");
                    }
                    put_c('\n');
                } else {
                    put_s(sep); put_s(": FAILED open or read");
                    put_c('\n');
                }
            }
            col = 0;
        } else if (ch != '\r' && col < (int)sizeof(line) - 1) {
            line[col++] = (char)ch;
        }
    }
    if (fd > 0) uaos_close((int)fd);
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"check", 'c', no_argument},
        {"zero",  'z', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "czw", long_opts, &li)) != -1) {
        switch (opt) {
            case 'c': opt_check = 1; break;
            case 'z': opt_zero = 1; break;
            default:  break;
        }
    }
    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) {
        if (opt_check) check_file("-");
        else md5sum_file("-");
        return 0;
    }
    for (int i = 0; i < nfiles; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (!fname) continue;
        if (opt_check) check_file(fname);
        else md5sum_file(fname);
    }
    return 0;
}
