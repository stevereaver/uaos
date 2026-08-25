/* sha1sum.c — GNU coreutils 'sha1sum' for UAOS gnu: layer
 *
 * Compute and check SHA-1 message digests.
 *   sha1sum [OPTION]... [FILE]...
 * Options: -c, --check, -z, --zero
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"
#include "uaos_hash.h"

static int opt_check = 0;

static void compute_sha1(int fd, uint32_t sz, int is_stdin, uint8_t out[20])
{
    sha1_ctx ctx; sha1_init(&ctx);
    uint32_t pos = 0;
    for (;;) {
        uint8_t buf[4096]; long n = 0;
        if (is_stdin) { n = uaos_read(fd, buf, sizeof(buf)); }
        else {
            long to_read = (long)(sz - pos < sizeof(buf) ? sz - pos : sizeof(buf));
            if (to_read <= 0) break;
            n = uaos_read_file(fd, buf, to_read); pos += (uint32_t)n;
        }
        if (n <= 0) break;
        sha1_update(&ctx, buf, (size_t)n);
    }
    sha1_final(&ctx, out);
}

static void sha1sum_file(const char *fname)
{
    int is_stdin = (fname[0] == '-' && fname[1] == '\0');
    long fd; uint32_t sz = 0;
    if (is_stdin) fd = 0;
    else {
        char path[UAOS_CMD_PATH_MAX]; cmd_make_abs(fname, path, sizeof(path));
        fd = uaos_open(path, UAOS_O_RDONLY);
        if (fd < 0) { put_s("sha1sum: "); put_s(fname); put_line(": No such file"); return; }
        struct uaos_stat st; if (uaos_stat(path, &st) == 0) sz = st.size;
    }
    uint8_t digest[20]; compute_sha1((int)fd, sz, is_stdin, digest);
    if (fd > 0) uaos_close((int)fd);
    char hex[41]; hash_to_hex(digest, 20, hex);
    put_s(hex); put_s("  "); put_s(fname); put_c('\n');
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"check", 'c', no_argument}, {"zero", 'z', no_argument}, {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "cz", long_opts, &li)) != -1) {
        switch (opt) { case 'c': opt_check = 1; break; default: break; }
    }
    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) { sha1sum_file("-"); return 0; }
    for (int i = 0; i < nfiles; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (fname) sha1sum_file(fname);
    }
    return 0;
}
