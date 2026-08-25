/* mv.c — GNU coreutils 'mv' for UAOS gnu: layer
 *
 * Move or rename files.
 *   mv [OPTION]... SOURCE... DEST
 * Options: -f, --force, -i, --interactive, -n, --no-clobber, -v, --verbose, -u, --update
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_force = 0;
static int opt_verbose = 0;
static int opt_no_clobber = 0;

static int do_move(const char *src, const char *dst)
{
    char spath[UAOS_CMD_PATH_MAX], dpath[UAOS_CMD_PATH_MAX];
    cmd_make_abs(src, spath, sizeof(spath));
    cmd_make_abs(dst, dpath, sizeof(dpath));

    struct uaos_stat st;
    if (uaos_stat(spath, &st) != 0) {
        put_s("mv: cannot stat '"); put_s(src); put_line("': No such file");
        return 1;
    }

    /* if dst is a directory, append src basename */
    struct uaos_stat dst_st;
    if (uaos_stat(dpath, &dst_st) == 0 && dst_st.is_dir) {
        const char *p = src;
        const char *last = src;
        while (*p) { if (*p == '/' || *p == ':') last = p + 1; p++; }
        cmd_join_path(dpath, last, dpath, sizeof(dpath));
    }

    if (opt_no_clobber) {
        struct uaos_stat check;
        if (uaos_stat(dpath, &check) == 0) return 0;
    }

    /* try rename first */
    if (uaos_rename(spath, dpath) == 0) {
        if (opt_verbose) {
            put_s("'"); put_s(src); put_s("' -> '"); put_s(dst); put_line("'");
        }
        return 0;
    }

    /* rename failed — fall back to copy + delete */
    struct uaos_stat src_st;
    if (uaos_stat(spath, &src_st) != 0) return 1;

    if (src_st.is_dir) {
        /* copy directory recursively then delete */
        put_s("mv: cross-device move of '"); put_s(src);
        put_line("' not supported");
        return 1;
    }

    long sfd = uaos_open(spath, UAOS_O_RDONLY);
    if (sfd < 0) return 1;
    long dfd = uaos_open(dpath, UAOS_O_WRONLY | UAOS_O_CREAT | UAOS_O_TRUNC);
    if (dfd < 0) { uaos_close((int)sfd); return 1; }

    uint32_t pos = 0;
    for (;;) {
        uint8_t buf[4096];
        long to_read = (long)(src_st.size - pos < sizeof(buf) ? src_st.size - pos : sizeof(buf));
        if (to_read <= 0) break;
        long n = uaos_read_file((int)sfd, buf, to_read);
        if (n <= 0) break;
        pos += (uint32_t)n;
        long off = 0;
        while (off < n) {
            long w = uaos_write_file((int)dfd, buf + off, n - off);
            if (w <= 0) break;
            off += w;
        }
    }
    uaos_close((int)sfd);
    uaos_close((int)dfd);
    uaos_delete(spath);

    if (opt_verbose) {
        put_s("'"); put_s(src); put_s("' -> '"); put_s(dst); put_line("'");
    }
    return 0;
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"force",      'f', no_argument},
        {"interactive",'i', no_argument},
        {"verbose",    'v', no_argument},
        {"no-clobber", 'n', no_argument},
        {"update",     'u', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "fivnu", long_opts, &li)) != -1) {
        switch (opt) {
            case 'f': opt_force = 1; break;
            case 'i': break;
            case 'v': opt_verbose = 1; break;
            case 'n': opt_no_clobber = 1; break;
            case 'u': break;
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    if (nops < 2) { put_line("mv: missing operand"); return 1; }

    const char *dst = uaos_operand(argc, argv, nops - 1);
    char dpath[UAOS_CMD_PATH_MAX];
    cmd_make_abs(dst, dpath, sizeof(dpath));
    struct uaos_stat dst_st;
    int dst_is_dir = (uaos_stat(dpath, &dst_st) == 0 && dst_st.is_dir);

    if (nops > 2 && !dst_is_dir) {
        put_s("mv: target '"); put_s(dst); put_line("' is not a directory");
        return 1;
    }

    int rc = 0;
    for (int i = 0; i < nops - 1; i++) {
        const char *src = uaos_operand(argc, argv, i);
        if (src) { if (do_move(src, dst) != 0) rc = 1; }
    }
    return rc;
}
