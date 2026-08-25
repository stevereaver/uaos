/* du.c — GNU coreutils 'du' for UAOS gnu: layer
 *
 * Estimate file space usage.
 *   du [OPTION]... [FILE]...
 * Options: -h, --human-readable, -s, --summarize, -a, --all, -k, -b, --bytes
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_human = 0;
static int opt_summarize = 0;
static int opt_all = 0;
static int opt_bytes = 0;

static void format_size(uint32_t sz, char *buf, int max)
{
    if (opt_bytes) { uint_to_dec(sz, buf, max); return; }
    if (opt_human) {
        if (sz >= 1073741824U) { uint_to_dec(sz / 1073741824U, buf, max); uaos_strlcat(buf, "G", max); }
        else if (sz >= 1048576U) { uint_to_dec(sz / 1048576U, buf, max); uaos_strlcat(buf, "M", max); }
        else if (sz >= 1024) { uint_to_dec(sz / 1024, buf, max); uaos_strlcat(buf, "K", max); }
        else { uint_to_dec(sz, buf, max); }
    } else {
        uint_to_dec((sz + 1023) / 1024, buf, max);
    }
}

static uint32_t du_walk(const char *path, int depth)
{
    struct uaos_stat st;
    if (uaos_stat(path, &st) != 0) return 0;

    if (!st.is_dir) {
        if (opt_all || (depth == 0 && opt_summarize)) {
            char szbuf[16];
            format_size(st.size, szbuf, sizeof(szbuf));
            put_s(szbuf); put_s("\t"); put_s(path); put_c('\n');
        }
        return st.size;
    }

    uint32_t total = 0;
    long dd = uaos_opendir(path);
    if (dd < 0) return 0;
    struct uaos_dirent ent;
    while (uaos_readdir((int)dd, &ent) > 0) {
        if (ent.name[0] == '.' && (ent.name[1] == '\0' ||
            (ent.name[1] == '.' && ent.name[2] == '\0')))
            continue;
        char child[UAOS_CMD_PATH_MAX];
        cmd_join_path(path, ent.name, child, sizeof(child));
        total += du_walk(child, depth + 1);
    }
    uaos_closedir((int)dd);

    if (!opt_summarize || depth == 0) {
        char szbuf[16];
        format_size(total, szbuf, sizeof(szbuf));
        put_s(szbuf); put_s("\t"); put_s(path); put_c('\n');
    }
    return total;
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"human-readable", 'h', no_argument},
        {"summarize",      's', no_argument},
        {"all",            'a', no_argument},
        {"bytes",          'b', no_argument},
        {"kibibytes",      'k', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "hsakb", long_opts, &li)) != -1) {
        switch (opt) {
            case 'h': opt_human = 1; break;
            case 's': opt_summarize = 1; break;
            case 'a': opt_all = 1; break;
            case 'b': opt_bytes = 1; break;
            case 'k': break; /* default is already 1K */
            default:  return 1;
        }
    }

    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) {
        char cwd[UAOS_CMD_PATH_MAX]; uaos_getcwd(cwd, sizeof(cwd));
        du_walk(cwd, 0);
    } else {
        for (int i = 0; i < nfiles; i++) {
            const char *fname = uaos_operand(argc, argv, i);
            if (fname) {
                char path[UAOS_CMD_PATH_MAX]; cmd_make_abs(fname, path, sizeof(path));
                du_walk(path, 0);
            }
        }
    }
    return 0;
}
