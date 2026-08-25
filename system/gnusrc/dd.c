/* dd.c — GNU coreutils 'dd' for UAOS gnu: layer
 *
 * Convert and copy a file.
 *   dd [OPERAND]...
 * Operands: if=FILE, of=FILE, bs=N, ibs=N, obs=N, count=N, skip=N, seek=N,
 *           conv=notrunc,sync,ucase,lcase, swab, noerror
 * Suffixes: c=1, w=2, b=512, kB=1000, K=1024, MB=1000000, M=1048576, etc.
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

/* dd uses OPERAND=VALUE syntax, not standard getopt. We parse argv directly. */

static long parse_dd_size(const char *s)
{
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    if (*s == 'c') v *= 1;
    else if (*s == 'w') v *= 2;
    else if (*s == 'b') v *= 512;
    else if (s[0] == 'k' && s[1] == 'B') v *= 1000;
    else if (*s == 'K') v *= 1024;
    else if (s[0] == 'M' && s[1] == 'B') v *= 1000000;
    else if (*s == 'M') v *= 1048576;
    else if (s[0] == 'G' && s[1] == 'B') v *= 1000000000L;
    else if (*s == 'G') v *= 1073741824L;
    return v;
}

static int streq(const char *a, const char *b)
{
    return uaos_strcmp(a, b) == 0;
}

int main(int argc, const char **argv)
{
    const char *ifname = NULL;
    const char *ofname = NULL;
    long bs = 512;
    long ibs = 0, obs = 0;
    long count = -1;
    long skip = 0, seek = 0;
    int conv_notrunc = 0;
    int conv_sync = 0;
    int conv_ucase = 0;
    int conv_lcase = 0;
    int conv_swab = 0;
    int conv_noerror = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (uaos_strncmp(arg, "if=", 3) == 0) ifname = arg + 3;
        else if (uaos_strncmp(arg, "of=", 3) == 0) ofname = arg + 3;
        else if (uaos_strncmp(arg, "bs=", 3) == 0) { bs = parse_dd_size(arg + 3); ibs = obs = bs; }
        else if (uaos_strncmp(arg, "ibs=", 4) == 0) ibs = parse_dd_size(arg + 4);
        else if (uaos_strncmp(arg, "obs=", 4) == 0) obs = parse_dd_size(arg + 4);
        else if (uaos_strncmp(arg, "count=", 6) == 0) count = parse_dd_size(arg + 6);
        else if (uaos_strncmp(arg, "skip=", 5) == 0) skip = parse_dd_size(arg + 5);
        else if (uaos_strncmp(arg, "seek=", 5) == 0) seek = parse_dd_size(arg + 5);
        else if (uaos_strncmp(arg, "conv=", 5) == 0) {
            const char *p = arg + 5;
            while (*p) {
                const char *start = p;
                while (*p && *p != ',') p++;
                int len = (int)(p - start);
                if (len == 7 && uaos_strncmp(start, "notrunc", 7) == 0) conv_notrunc = 1;
                else if (len == 4 && uaos_strncmp(start, "sync", 4) == 0) conv_sync = 1;
                else if (len == 5 && uaos_strncmp(start, "ucase", 5) == 0) conv_ucase = 1;
                else if (len == 5 && uaos_strncmp(start, "lcase", 5) == 0) conv_lcase = 1;
                else if (len == 4 && uaos_strncmp(start, "swab", 4) == 0) conv_swab = 1;
                else if (len == 7 && uaos_strncmp(start, "noerror", 7) == 0) conv_noerror = 1;
                if (*p == ',') p++;
            }
        }
    }

    if (ibs == 0) ibs = bs;
    if (obs == 0) obs = bs;

    long ifd = 0; /* default stdin */
    int is_stdin = 1;
    uint32_t isz = 0;
    if (ifname) {
        char path[UAOS_CMD_PATH_MAX];
        cmd_make_abs(ifname, path, sizeof(path));
        ifd = uaos_open(path, UAOS_O_RDONLY);
        if (ifd < 0) { put_s("dd: failed to open '"); put_s(ifname); put_line("'"); return 1; }
        is_stdin = 0;
        struct uaos_stat st;
        if (uaos_stat(path, &st) == 0) isz = st.size;
    }

    long ofd = 1; /* default stdout */
    int is_stdout = 1;
    if (ofname) {
        char path[UAOS_CMD_PATH_MAX];
        cmd_make_abs(ofname, path, sizeof(path));
        int flags = UAOS_O_WRONLY | UAOS_O_CREAT;
        if (!conv_notrunc) flags |= UAOS_O_TRUNC;
        ofd = uaos_open(path, flags);
        if (ofd < 0) { put_s("dd: failed to open '"); put_s(ofname); put_line("'"); return 1; }
        is_stdout = 0;
    }

    /* skip input blocks */
    for (long i = 0; i < skip; i++) {
        uint8_t tmp[4096];
        long to_read = ibs;
        while (to_read > 0) {
            long chunk = to_read < (long)sizeof(tmp) ? to_read : (long)sizeof(tmp);
            long n;
            if (is_stdin) n = uaos_read((int)ifd, tmp, chunk);
            else n = uaos_read_file((int)ifd, tmp, chunk);
            if (n <= 0) break;
            to_read -= n;
        }
    }

    /* seek output blocks */
    for (long i = 0; i < seek; i++) {
        if (!is_stdout) {
            uint8_t tmp[4096];
            uaos_memset(tmp, 0, sizeof(tmp));
            long to_write = obs;
            while (to_write > 0) {
                long chunk = to_write < (long)sizeof(tmp) ? to_write : (long)sizeof(tmp);
                uaos_write_file((int)ofd, tmp, chunk);
                to_write -= chunk;
            }
        }
    }

    uint8_t *buf = (uint8_t *)uaos_alloc(ibs > obs ? ibs : obs);
    if (!buf) { put_line("dd: out of memory"); return 1; }

    long blocks_in = 0, blocks_out = 0;
    long blocks_done = 0;

    while (count < 0 || blocks_done < count) {
        long n = 0;
        if (is_stdin) n = uaos_read((int)ifd, buf, ibs);
        else {
            if (isz > 0) {
                /* use read_file for file-backed input */
                n = uaos_read_file((int)ifd, buf, ibs);
            } else {
                n = uaos_read((int)ifd, buf, ibs);
            }
        }
        if (n <= 0) break;

        /* pad with zeros if sync and short read */
        if (conv_sync && n < ibs) {
            uaos_memset(buf + n, 0, ibs - n);
            n = ibs;
        }

        /* apply conversions */
        if (conv_swab) {
            for (long i = 0; i + 1 < n; i += 2) {
                uint8_t t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t;
            }
        }
        if (conv_ucase) {
            for (long i = 0; i < n; i++)
                if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;
        }
        if (conv_lcase) {
            for (long i = 0; i < n; i++)
                if (buf[i] >= 'A' && buf[i] <= 'Z') buf[i] += 32;
        }

        /* write */
        long off = 0;
        while (off < n) {
            long w;
            if (is_stdout) w = uaos_write((int)ofd, buf + off, n - off);
            else w = uaos_write_file((int)ofd, buf + off, n - off);
            if (w <= 0) break;
            off += w;
        }
        blocks_in++;
        blocks_out++;
        blocks_done++;
    }

    /* report */
    char buf2[16];
    int_to_dec((int32_t)blocks_in, buf2, sizeof(buf2));
    put_s(buf2);
    put_s("+0 records in\n");
    int_to_dec((int32_t)blocks_out, buf2, sizeof(buf2));
    put_s(buf2);
    put_s("+0 records out\n");

    if (ifd > 0) uaos_close((int)ifd);
    if (ofd > 0) uaos_close((int)ofd);
    return 0;
}
