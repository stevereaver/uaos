/* stat.c — GNU coreutils 'stat' for UAOS gnu: layer
 *
 * Display file or filesystem status.
 *   stat [OPTION]... FILE...
 * Options: -f, --file-system, -c FORMAT, --format=FORMAT, -L, --dereference,
 *          -t, --terse
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_fs = 0;
static int opt_terse = 0;
static const char *opt_format = NULL;

static void stat_file(const char *fname)
{
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(fname, path, sizeof(path));
    struct uaos_stat st;
    if (uaos_stat(path, &st) != 0) {
        put_s("stat: cannot stat '");
        put_s(fname);
        put_line("': No such file or directory");
        return;
    }

    if (opt_terse) {
        char buf[16];
        put_s(fname); put_c(' ');
        uint_to_dec(st.size, buf, sizeof(buf)); put_s(buf); put_c(' ');
        uint_to_dec(st.protection, buf, sizeof(buf)); put_s(buf);
        put_c('\n');
        return;
    }

    if (opt_format) {
        /* simple format substitution */
        const char *p = opt_format;
        while (*p) {
            if (*p == '%') {
                p++;
                char buf[16];
                switch (*p) {
                    case 'n': put_s(fname); break;
                    case 's': uint_to_dec(st.size, buf, sizeof(buf)); put_s(buf); break;
                    case 'F':
                        put_s(st.is_dir ? "directory" : "regular file");
                        break;
                    case 'a':
                        uint_to_dec(st.protection, buf, sizeof(buf));
                        put_s(buf);
                        break;
                    case 'U': put_s("root"); break;
                    case 'G': put_s("root"); break;
                    case 'Y': uint_to_dec(st.mtime, buf, sizeof(buf)); put_s(buf); break;
                    case '%': put_c('%'); break;
                    case '\0': p--; break;
                    default: put_c('%'); put_c(*p); break;
                }
            } else {
                put_c(*p);
            }
            p++;
        }
        if (opt_format[(int)uaos_strlen(opt_format) - 1] != '\n')
            put_c('\n');
        return;
    }

    /* default output */
    put_s("  File: "); put_s(fname); put_c('\n');
    put_s("  Size: "); char buf[16]; uint_to_dec(st.size, buf, sizeof(buf)); put_s(buf);
    put_s("\t\t");
    put_s("Type: "); put_s(st.is_dir ? "directory" : "regular file"); put_c('\n');
    put_s("Access: (0");
    uint_to_dec(st.protection, buf, sizeof(buf)); put_s(buf);
    put_s(")  Uid: (0/root)  Gid: (0/root)\n");
    char datebuf[20];
    cmd_fmt_mtime(st.mtime, datebuf, sizeof(datebuf));
    put_s("Modify: "); put_s(datebuf); put_c('\n');
}

static void stat_fs(const char *fname)
{
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(fname, path, sizeof(path));
    uint32_t total = 0, used = 0;
    if (uaos_getvolumeinfo(path, &total, &used) != 0) {
        put_s("stat: cannot read filesystem info for '");
        put_s(fname); put_line("'");
        return;
    }
    put_s("  File: \""); put_s(fname); put_s("\"\n");
    char buf[16];
    put_s("  Size: "); uint_to_dec(total - used, buf, sizeof(buf)); put_s(buf);
    put_s("  Type: ramfs\n");
    put_s("  Total: "); uint_to_dec(total, buf, sizeof(buf)); put_s(buf); put_c('\n');
    put_s("  Used: ");  uint_to_dec(used, buf, sizeof(buf));  put_s(buf); put_c('\n');
    put_s("  Free: ");  uint_to_dec(total - used, buf, sizeof(buf)); put_s(buf); put_c('\n');
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"file-system",  'f', no_argument},
        {"format",       'c', required_argument},
        {"dereference",  'L', no_argument},
        {"terse",        't', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "fc:Lt", long_opts, &li)) != -1) {
        switch (opt) {
            case 'f': opt_fs = 1; break;
            case 'c': opt_format = g_optarg; break;
            case 'L': break; /* dereference — always follow in UAOS */
            case 't': opt_terse = 1; break;
            default:  return 1;
        }
    }
    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) { put_line("stat: missing operand"); return 1; }
    for (int i = 0; i < nfiles; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (fname) { if (opt_fs) stat_fs(fname); else stat_file(fname); }
    }
    return 0;
}
