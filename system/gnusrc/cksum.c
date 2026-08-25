/* cksum.c — GNU coreutils 'cksum' for UAOS gnu: layer
 *
 * Compute CRC32 checksum and byte count.
 *   cksum [OPTION]... [FILE]...
 * Options: --algorithm=ALG (crc32 default, crc32b, sha*, md5, etc.)
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"
#include "uaos_hash.h"

static void cksum_file(int fd, uint32_t sz, int is_stdin, const char *fname, int multi)
{
    uint32_t crc = 0;
    uint32_t bytes = 0;
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
        crc = crc32_update(crc, buf, (size_t)n);
        bytes += (uint32_t)n;
    }
    /* include the length in the CRC (POSIX cksum) */
    uint32_t len = bytes;
    while (len) {
        crc = crc32_update(crc, (const uint8_t *)&len, 1);
        len >>= 8;
    }
    crc = ~crc;

    char cbuf[12], bbuf[12];
    uint_to_dec(crc, cbuf, sizeof(cbuf));
    uint_to_dec(bytes, bbuf, sizeof(bbuf));
    put_s(cbuf); put_s(" "); put_s(bbuf);
    if (multi && fname) { put_s(" "); put_s(fname); }
    put_c('\n');
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"algorithm", 'a', required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "a:", long_opts, &li)) != -1) {
        switch (opt) {
            case 'a': /* only crc32 supported for now */ break;
            default:  return 1;
        }
    }
    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) { cksum_file(0, 0, 1, NULL, 0); return 0; }
    for (int i = 0; i < nfiles; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (!fname) continue;
        int is_stdin = (fname[0] == '-' && fname[1] == '\0');
        long fd; uint32_t sz = 0;
        if (is_stdin) fd = 0;
        else {
            char path[UAOS_CMD_PATH_MAX]; cmd_make_abs(fname, path, sizeof(path));
            fd = uaos_open(path, UAOS_O_RDONLY);
            if (fd < 0) { put_s("cksum: "); put_s(fname); put_line(": No such file"); continue; }
            struct uaos_stat st; if (uaos_stat(path, &st) == 0) sz = st.size;
        }
        cksum_file((int)fd, sz, is_stdin, fname, nfiles > 1);
        if (fd > 0) uaos_close((int)fd);
    }
    return 0;
}
