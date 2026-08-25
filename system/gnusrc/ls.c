/* ls.c — GNU coreutils 'ls' for UAOS gnu: layer
 *
 * List directory contents.
 *   ls [OPTION]... [FILE]...
 * Options: -l, --long, -a, --all, -A, --almost-all, -1, -r, --reverse,
 *          -S, --sort=size, -t, --sort=time, -h, --human-readable,
 *          -i, --inode, -R, --recursive, -d, --directory, -F, --classify,
 *          -p, --indicator-style=slash, -n, --numeric-uid-gid
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

#define LS_MAX_ENTRIES 512

static int opt_long = 0;
static int opt_all = 0;       /* -a: include . and .. */
static int opt_almost_all = 0;/* -A: include hidden but not . and .. */
static int opt_one = 0;       /* -1: one per line */
static int opt_reverse = 0;
static int opt_sort_size = 0;
static int opt_sort_time = 0;
static int opt_human = 0;
static int opt_recursive = 0;
static int opt_directory = 0;
static int opt_classify = 0;
static int opt_slash = 0;
static int opt_numeric = 0;

typedef struct {
    char name[32];
    uint32_t size;
    uint8_t  is_dir;
    uint16_t protection;
    uint32_t mtime;
} LsEntry;

static LsEntry g_entries[LS_MAX_ENTRIES];
static int g_entry_count = 0;

static int ls_cmp(const LsEntry *a, const LsEntry *b)
{
    int r = 0;
    if (opt_sort_size) {
        if (a->size > b->size) r = -1;
        else if (a->size < b->size) r = 1;
    } else if (opt_sort_time) {
        if (a->mtime > b->mtime) r = -1;
        else if (a->mtime < b->mtime) r = 1;
    } else {
        r = uaos_strcmp(a->name, b->name);
    }
    if (opt_reverse) r = -r;
    return r;
}

static void ls_sort(void)
{
    for (int i = 0; i < g_entry_count - 1; i++)
        for (int j = 0; j < g_entry_count - 1 - i; j++)
            if (ls_cmp(&g_entries[j], &g_entries[j + 1]) > 0) {
                LsEntry tmp = g_entries[j];
                g_entries[j] = g_entries[j + 1];
                g_entries[j + 1] = tmp;
            }
}

static void format_size(uint32_t sz, char *buf, int max)
{
    if (opt_human && sz >= 1073741824U) {
        uint_to_dec(sz / 1073741824U, buf, max);
        uaos_strlcat(buf, "G", max);
    } else if (opt_human && sz >= 1048576U) {
        uint_to_dec(sz / 1048576U, buf, max);
        uaos_strlcat(buf, "M", max);
    } else if (opt_human && sz >= 1024) {
        uint_to_dec(sz / 1024, buf, max);
        uaos_strlcat(buf, "K", max);
    } else {
        uint_to_dec(sz, buf, max);
    }
}

static void format_perms(uint16_t prot, char *buf)
{
    /* AmigaDOS protection bits (inverted: set = disallowed) */
    buf[0] = 'r'; buf[1] = (prot & UAOS_FIBF_READ) ? '-' : 'r';
    buf[2] = 'w'; buf[3] = (prot & UAOS_FIBF_WRITE) ? '-' : 'w';
    buf[4] = 'x'; buf[5] = (prot & UAOS_FIBF_EXECUTE) ? '-' : 'x';
    buf[6] = 'd'; buf[7] = (prot & UAOS_FIBF_DELETE) ? '-' : 'd';
    buf[8] = '\0';
}

static void print_entry(LsEntry *e, int multi, const char *dirpath)
{
    if (opt_long) {
        char perm[10];
        format_perms(e->protection, perm);
        char szbuf[16];
        format_size(e->size, szbuf, sizeof(szbuf));
        char datebuf[20];
        cmd_fmt_mtime(e->mtime, datebuf, sizeof(datebuf));

        put_s(e->is_dir ? "d" : "-");
        put_s(perm);
        put_s("  ");
        /* pad size to 10 right-justified */
        int slen = (int)uaos_strlen(szbuf);
        for (int i = slen; i < 10; i++) put_c(' ');
        put_s(szbuf);
        put_s("  ");
        put_s(opt_numeric ? "0" : "root");
        put_s("  ");
        put_s(opt_numeric ? "0" : "root");
        put_s("  ");
        put_s(datebuf);
        put_s("  ");
        put_s(e->name);
        if ((opt_classify || opt_slash) && e->is_dir) put_c('/');
        if (opt_classify && !e->is_dir && (e->protection & UAOS_FIBF_EXECUTE) == 0)
            put_c('*');
        put_c('\n');
    } else if (opt_one) {
        put_s(e->name);
        if ((opt_classify || opt_slash) && e->is_dir) put_c('/');
        put_c('\n');
    } else {
        /* columnar output — simple space-separated */
        put_s(e->name);
        if ((opt_classify || opt_slash) && e->is_dir) put_c('/');
        put_s("  ");
    }
}

static void ls_directory(const char *path, int print_header)
{
    long dd = uaos_opendir(path);
    if (dd < 0) {
        put_s("ls: cannot access '");
        put_s(path);
        put_line("'");
        return;
    }

    g_entry_count = 0;
    struct uaos_dirent ent;
    while (uaos_readdir((int)dd, &ent) > 0 && g_entry_count < LS_MAX_ENTRIES) {
        /* skip hidden unless -a or -A */
        if (ent.name[0] == '.') {
            if (!opt_all && !opt_almost_all) continue;
            if (opt_almost_all) {
                /* skip . and .. */
                if ((ent.name[1] == '\0') ||
                    (ent.name[1] == '.' && ent.name[2] == '\0'))
                    continue;
            }
        }
        uaos_strcpy(g_entries[g_entry_count].name, ent.name);
        g_entries[g_entry_count].size = ent.size;
        g_entries[g_entry_count].is_dir = ent.is_dir;
        g_entries[g_entry_count].protection = ent.protection;
        g_entries[g_entry_count].mtime = ent.mtime;
        g_entry_count++;
    }
    uaos_closedir((int)dd);

    ls_sort();

    if (print_header) {
        put_c('\n');
        put_s(path);
        put_line(":");
    }

    for (int i = 0; i < g_entry_count; i++)
        print_entry(&g_entries[i], 0, path);
    if (!opt_long && !opt_one && g_entry_count > 0)
        put_c('\n');

    if (opt_recursive) {
        for (int i = 0; i < g_entry_count; i++) {
            if (g_entries[i].is_dir && g_entries[i].name[0] != '.') {
                char subpath[UAOS_CMD_PATH_MAX];
                cmd_join_path(path, g_entries[i].name, subpath, sizeof(subpath));
                ls_directory(subpath, 1);
            }
        }
    }
}

static void ls_file(const char *fname)
{
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(fname, path, sizeof(path));
    struct uaos_stat st;
    if (uaos_stat(path, &st) != 0) {
        put_s("ls: cannot access '");
        put_s(fname);
        put_line("'");
        return;
    }
    if (st.is_dir && !opt_directory) {
        ls_directory(path, 0);
    } else {
        LsEntry e;
        /* extract the name from the path */
        const char *p = path;
        const char *last = path;
        while (*p) { if (*p == '/' || *p == ':') last = p + 1; p++; }
        uaos_strcpy(e.name, last);
        e.size = st.size;
        e.is_dir = st.is_dir;
        e.protection = st.protection;
        e.mtime = st.mtime;
        print_entry(&e, 0, NULL);
        if (!opt_long && !opt_one) put_c('\n');
    }
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"long",           'l', no_argument},
        {"all",            'a', no_argument},
        {"almost-all",     'A', no_argument},
        {"reverse",        'r', no_argument},
        {"sort",            0,  required_argument},
        {"human-readable", 'h', no_argument},
        {"recursive",      'R', no_argument},
        {"directory",      'd', no_argument},
        {"classify",       'F', no_argument},
        {"inode",          'i', no_argument},
        {"numeric-uid-gid",'n', no_argument},
        {"indicator-style", 0, required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "laAr1StRhRdFipn", long_opts, &li)) != -1) {
        switch (opt) {
            case 'l': opt_long = 1; break;
            case 'a': opt_all = 1; break;
            case 'A': opt_almost_all = 1; break;
            case '1': opt_one = 1; break;
            case 'r': opt_reverse = 1; break;
            case 'S': opt_sort_size = 1; break;
            case 't': opt_sort_time = 1; break;
            case 'R': opt_recursive = 1; break;
            case 'd': opt_directory = 1; break;
            case 'F': opt_classify = 1; break;
            case 'p': opt_slash = 1; break;
            case 'h': opt_human = 1; break;
            case 'n': opt_numeric = 1; opt_long = 1; break;
            case 'i': break; /* inode — not supported, ignore */
            case UAOS_GO_LONG + 0: /* --sort= */
                if (g_optarg) {
                    if (g_optarg[0] == 's') opt_sort_size = 1;
                    else if (g_optarg[0] == 't') opt_sort_time = 1;
                }
                break;
            case UAOS_GO_LONG + 1: /* --indicator-style= */
                if (g_optarg) {
                    if (g_optarg[0] == 's') opt_slash = 1;
                    else if (g_optarg[0] == 'f') opt_classify = 1;
                }
                break;
            default:  return 1;
        }
    }

    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) {
        char cwd[UAOS_CMD_PATH_MAX];
        uaos_getcwd(cwd, sizeof(cwd));
        ls_directory(cwd, 0);
    } else {
        int multi = (nfiles > 1);
        for (int i = 0; i < nfiles; i++) {
            const char *fname = uaos_operand(argc, argv, i);
            if (!fname) continue;
            if (multi && !opt_long) {
                if (i > 0) put_c('\n');
                put_s(fname);
                put_line(":");
            }
            ls_file(fname);
        }
    }
    return 0;
}
