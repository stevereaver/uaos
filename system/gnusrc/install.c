/* install.c — GNU coreutils 'install' for UAOS gnu: layer
 *
 * Copy files and set attributes.
 *   install [OPTION]... SOURCE... DEST
 * Options: -d, --directory, -m MODE, --mode=MODE, -v, --verbose,
 *          -t DIR, --target-directory=DIR, -D
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_directory = 0;
static int opt_verbose = 0;
static uint16_t opt_mode = 0;
static const char *opt_target_dir = NULL;
static int opt_create_dirs = 0;

static int do_install(const char *src, const char *dst)
{
    char spath[UAOS_CMD_PATH_MAX], dpath[UAOS_CMD_PATH_MAX];
    cmd_make_abs(src, spath, sizeof(spath));
    cmd_make_abs(dst, dpath, sizeof(dpath));

    /* if dst is a directory, append src basename */
    struct uaos_stat dst_st;
    if (uaos_stat(dpath, &dst_st) == 0 && dst_st.is_dir) {
        const char *p = src;
        const char *last = src;
        while (*p) { if (*p == '/' || *p == ':') last = p + 1; p++; }
        cmd_join_path(dpath, last, dpath, sizeof(dpath));
    }

    long sfd = uaos_open(spath, UAOS_O_RDONLY);
    if (sfd < 0) { put_s("install: cannot open '"); put_s(src); put_line("'"); return 1; }
    struct uaos_stat st;
    uaos_stat(spath, &st);
    long dfd = uaos_open(dpath, UAOS_O_WRONLY | UAOS_O_CREAT | UAOS_O_TRUNC);
    if (dfd < 0) { put_s("install: cannot create '"); put_s(dst); put_line("'"); uaos_close((int)sfd); return 1; }

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

    if (opt_mode != 0) uaos_setprotection(dpath, opt_mode);

    if (opt_verbose) {
        put_s("'"); put_s(src); put_s("' -> '"); put_s(dst); put_line("'");
    }
    return 0;
}

static int make_dirs(const char *dir)
{
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(dir, path, sizeof(path));
    int len = (int)uaos_strlen(path);
    for (int i = 1; i < len; i++) {
        if (path[i] == '/' || path[i] == ':') {
            char save = path[i];
            path[i] = '\0';
            struct uaos_stat st;
            if (uaos_stat(path, &st) != 0) uaos_mkdir(path);
            path[i] = save;
        }
    }
    struct uaos_stat st;
    if (uaos_stat(path, &st) != 0) uaos_mkdir(path);
    if (opt_mode != 0) uaos_setprotection(path, opt_mode);
    return 0;
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"directory",          'd', no_argument},
        {"mode",               'm', required_argument},
        {"verbose",            'v', no_argument},
        {"target-directory",   't', required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "dm:vt:D", long_opts, &li)) != -1) {
        switch (opt) {
            case 'd': opt_directory = 1; break;
            case 'm':
                if (g_optarg) {
                    long v = 0; const char *p = g_optarg;
                    while (*p >= '0' && *p <= '7') { v = v * 8 + (*p - '0'); p++; }
                    opt_mode = (uint16_t)v;
                }
                break;
            case 'v': opt_verbose = 1; break;
            case 't': opt_target_dir = g_optarg; break;
            case 'D': opt_create_dirs = 1; break;
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);

    if (opt_directory) {
        if (nops == 0) { put_line("install: missing operand"); return 1; }
        for (int i = 0; i < nops; i++) {
            const char *dir = uaos_operand(argc, argv, i);
            if (dir) make_dirs(dir);
        }
        return 0;
    }

    if (nops < 2 && !opt_target_dir) {
        put_line("install: missing destination file operand");
        return 1;
    }

    if (opt_target_dir) {
        for (int i = 0; i < nops; i++) {
            const char *src = uaos_operand(argc, argv, i);
            if (src) do_install(src, opt_target_dir);
        }
        return 0;
    }

    const char *dst = uaos_operand(argc, argv, nops - 1);
    for (int i = 0; i < nops - 1; i++) {
        const char *src = uaos_operand(argc, argv, i);
        if (src) do_install(src, dst);
    }
    return 0;
}
