/* cp.c — GNU coreutils 'cp' for UAOS gnu: layer
 *
 * Copy files and directories.
 *   cp [OPTION]... SOURCE... DEST
 * Options: -r, -R, --recursive, -f, --force, -i, --interactive,
 *          -n, --no-clobber, -v, --verbose, -p, --preserve, -u, --update
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_recursive = 0;
static int opt_force = 0;
static int opt_verbose = 0;
static int opt_no_clobber = 0;
static int opt_preserve = 0;

static int copy_file(const char *src, const char *dst)
{
    char spath[UAOS_CMD_PATH_MAX], dpath[UAOS_CMD_PATH_MAX];
    cmd_make_abs(src, spath, sizeof(spath));
    cmd_make_abs(dst, dpath, sizeof(dpath));

    struct uaos_stat st;
    if (uaos_stat(spath, &st) != 0) {
        put_s("cp: cannot stat '"); put_s(src); put_line("': No such file");
        return 1;
    }

    if (st.is_dir && !opt_recursive) {
        put_s("cp: omitting directory '"); put_s(src); put_line("'");
        return 1;
    }

    if (st.is_dir) {
        /* check if dst exists, if not create it */
        struct uaos_stat dst_st;
        if (uaos_stat(dpath, &dst_st) != 0) {
            if (uaos_mkdir(dpath) != 0) {
                put_s("cp: cannot create directory '"); put_s(dst); put_line("'");
                return 1;
            }
        }
        long dd = uaos_opendir(spath);
        if (dd < 0) return 1;
        struct uaos_dirent ent;
        while (uaos_readdir((int)dd, &ent) > 0) {
            if (ent.name[0] == '.' && (ent.name[1] == '\0' ||
                (ent.name[1] == '.' && ent.name[2] == '\0'))) continue;
            char child_src[UAOS_CMD_PATH_MAX];
            char child_dst[UAOS_CMD_PATH_MAX];
            cmd_join_path(spath, ent.name, child_src, sizeof(child_src));
            cmd_join_path(dpath, ent.name, child_dst, sizeof(child_dst));
            copy_file(child_src, child_dst);
        }
        uaos_closedir((int)dd);
        return 0;
    }

    if (opt_no_clobber) {
        struct uaos_stat dst_st;
        if (uaos_stat(dpath, &dst_st) == 0) return 0;
    }

    /* check if dst is a directory — if so, append src basename */
    struct uaos_stat dst_st;
    if (uaos_stat(dpath, &dst_st) == 0 && dst_st.is_dir) {
        const char *p = src;
        const char *last = src;
        while (*p) { if (*p == '/' || *p == ':') last = p + 1; p++; }
        cmd_join_path(dpath, last, dpath, sizeof(dpath));
    }

    long sfd = uaos_open(spath, UAOS_O_RDONLY);
    if (sfd < 0) { put_s("cp: cannot open '"); put_s(src); put_line("'"); return 1; }
    long dfd = uaos_open(dpath, UAOS_O_WRONLY | UAOS_O_CREAT | UAOS_O_TRUNC);
    if (dfd < 0) { put_s("cp: cannot create '"); put_s(dst); put_line("'"); uaos_close((int)sfd); return 1; }

    uint32_t pos = 0;
    for (;;) {
        uint8_t buf[4096];
        long to_read = (long)(st.size - pos < sizeof(buf) ? st.size - pos : sizeof(buf));
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

    if (opt_preserve) {
        uaos_setprotection(dpath, st.protection);
    }

    if (opt_verbose) {
        put_s("'"); put_s(src); put_s("' -> '"); put_s(dst); put_line("'");
    }
    return 0;
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"recursive", 'r', no_argument},
        {"force",     'f', no_argument},
        {"interactive",'i',no_argument},
        {"verbose",   'v', no_argument},
        {"no-clobber",'n', no_argument},
        {"preserve",  'p', no_argument},
        {"update",    'u', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "rRfinvpu", long_opts, &li)) != -1) {
        switch (opt) {
            case 'r': case 'R': opt_recursive = 1; break;
            case 'f': opt_force = 1; break;
            case 'i': break; /* interactive — not supported, ignore */
            case 'v': opt_verbose = 1; break;
            case 'n': opt_no_clobber = 1; break;
            case 'p': opt_preserve = 1; break;
            case 'u': break; /* update — not supported, ignore */
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    if (nops < 2) { put_line("cp: missing file operand"); return 1; }

    const char *dst = uaos_operand(argc, argv, nops - 1);
    /* check if dst is a directory for multi-source */
    char dpath[UAOS_CMD_PATH_MAX];
    cmd_make_abs(dst, dpath, sizeof(dpath));
    struct uaos_stat dst_st;
    int dst_is_dir = (uaos_stat(dpath, &dst_st) == 0 && dst_st.is_dir);

    if (nops > 2 && !dst_is_dir) {
        put_s("cp: target '"); put_s(dst); put_line("' is not a directory");
        return 1;
    }

    int rc = 0;
    for (int i = 0; i < nops - 1; i++) {
        const char *src = uaos_operand(argc, argv, i);
        if (src) { if (copy_file(src, dst) != 0) rc = 1; }
    }
    return rc;
}
