/* sum.c — GNU coreutils 'sum' for UAOS gnu: layer
 *
 * Checksum and count blocks in a file.
 *   sum [OPTION]... [FILE]...
 * Options: -r (BSD default, 1K blocks), -s, --sysv (512-byte blocks, SysV checksum)
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_sysv = 0;

static void sum_bsd(int fd, uint32_t sz, int is_stdin, const char *fname, int multi)
{
    uint32_t checksum = 0;
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
        for (long i = 0; i < n; i++) {
            checksum = (checksum >> 1) | ((checksum & 1) << 15);
            checksum += buf[i];
        }
        bytes += (uint32_t)n;
    }
    uint32_t blocks = (bytes + 1023) / 1024;
    char cbuf[12], bbuf[12];
    uint_to_dec(checksum & 0xFFFF, cbuf, sizeof(cbuf));
    uint_to_dec(blocks, bbuf, sizeof(bbuf));
    put_s(cbuf); put_s(" "); put_s(bbuf);
    if (multi && fname) { put_s(" "); put_s(fname); }
    put_c('\n');
}

static void sum_sysv(int fd, uint32_t sz, int is_stdin, const char *fname, int multi)
{
    uint32_t checksum = 0;
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
        for (long i = 0; i < n; i++) checksum += buf[i];
        bytes += (uint32_t)n;
    }
    uint32_t r = (checksum & 0xFFFF) + ((checksum >> 16) & 0xFFFF);
    uint32_t final = (r & 0xFFFF) + (r >> 16);
    uint32_t blocks = (bytes + 511) / 512;
    char cbuf[12], bbuf[12];
    uint_to_dec(final, cbuf, sizeof(cbuf));
    uint_to_dec(blocks, bbuf, sizeof(bbuf));
    put_s(cbuf); put_s(" "); put_s(bbuf);
    if (multi && fname) { put_s(" "); put_s(fname); }
    put_c('\n');
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"sysv", 's', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "rs", long_opts, &li)) != -1) {
        switch (opt) {
            case 'r': opt_sysv = 0; break;
            case 's': opt_sysv = 1; break;
            default:  return 1;
        }
    }
    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) {
        if (opt_sysv) sum_sysv(0, 0, 1, NULL, 0);
        else sum_bsd(0, 0, 1, NULL, 0);
        return 0;
    }
    for (int i = 0; i < nfiles; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (!fname) continue;
        int is_stdin = (fname[0] == '-' && fname[1] == '\0');
        long fd; uint32_t sz = 0;
        if (is_stdin) fd = 0;
        else {
            char path[UAOS_CMD_PATH_MAX]; cmd_make_abs(fname, path, sizeof(path));
            fd = uaos_open(path, UAOS_O_RDONLY);
            if (fd < 0) { put_s("sum: "); put_s(fname); put_line(": No such file"); continue; }
            struct uaos_stat st; if (uaos_stat(path, &st) == 0) sz = st.size;
        }
        if (opt_sysv) sum_sysv((int)fd, sz, is_stdin, fname, nfiles > 1);
        else sum_bsd((int)fd, sz, is_stdin, fname, nfiles > 1);
        if (fd > 0) uaos_close((int)fd);
    }
    return 0;
}
