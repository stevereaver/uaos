/* vdir.c — GNU coreutils 'vdir' for UAOS gnu: layer
 *
 * Like 'ls -l' (long format by default).
 *   vdir [OPTION]... [FILE]...
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

#define VDIR_MAX 512

static int opt_all = 0;
static int opt_almost_all = 0;
static int opt_reverse = 0;
static int opt_sort_size = 0;
static int opt_sort_time = 0;
static int opt_recursive = 0;

typedef struct {
    char name[32];
    uint32_t size;
    uint8_t  is_dir;
    uint16_t protection;
    uint32_t mtime;
} VEntry;

static VEntry g_entries[VDIR_MAX];
static int g_count = 0;

static int vcmp(const VEntry *a, const VEntry *b)
{
    int r = 0;
    if (opt_sort_size) { r = (a->size > b->size) ? -1 : (a->size < b->size ? 1 : 0); }
    else if (opt_sort_time) { r = (a->mtime > b->mtime) ? -1 : (a->mtime < b->mtime ? 1 : 0); }
    else r = uaos_strcmp(a->name, b->name);
    if (opt_reverse) r = -r;
    return r;
}

static void vdir_sort(void)
{
    for (int i = 0; i < g_count - 1; i++)
        for (int j = 0; j < g_count - 1 - i; j++)
            if (vcmp(&g_entries[j], &g_entries[j + 1]) > 0) {
                VEntry t = g_entries[j]; g_entries[j] = g_entries[j + 1]; g_entries[j + 1] = t;
            }
}

static void vdir_directory(const char *path, int header)
{
    long dd = uaos_opendir(path);
    if (dd < 0) { put_s("vdir: cannot access '"); put_s(path); put_line("'"); return; }
    g_count = 0;
    struct uaos_dirent ent;
    while (uaos_readdir((int)dd, &ent) > 0 && g_count < VDIR_MAX) {
        if (ent.name[0] == '.') {
            if (!opt_all && !opt_almost_all) continue;
            if (opt_almost_all && (ent.name[1] == '\0' || (ent.name[1] == '.' && ent.name[2] == '\0'))) continue;
        }
        uaos_strcpy(g_entries[g_count].name, ent.name);
        g_entries[g_count].size = ent.size;
        g_entries[g_count].is_dir = ent.is_dir;
        g_entries[g_count].protection = ent.protection;
        g_entries[g_count].mtime = ent.mtime;
        g_count++;
    }
    uaos_closedir((int)dd);
    vdir_sort();
    if (header) { put_c('\n'); put_s(path); put_line(":"); }
    for (int i = 0; i < g_count; i++) {
        char perm[10];
        perm[0] = g_entries[i].is_dir ? 'd' : '-';
        perm[1] = (g_entries[i].protection & UAOS_FIBF_READ) ? '-' : 'r';
        perm[2] = (g_entries[i].protection & UAOS_FIBF_WRITE) ? '-' : 'w';
        perm[3] = (g_entries[i].protection & UAOS_FIBF_EXECUTE) ? '-' : 'x';
        perm[4] = (g_entries[i].protection & UAOS_FIBF_DELETE) ? '-' : 'd';
        perm[5] = '\0';
        char szbuf[16]; uint_to_dec(g_entries[i].size, szbuf, sizeof(szbuf));
        int slen = (int)uaos_strlen(szbuf);
        char datebuf[20]; cmd_fmt_mtime(g_entries[i].mtime, datebuf, sizeof(datebuf));
        put_s(perm); put_s("  ");
        for (int p = slen; p < 10; p++) put_c(' ');
        put_s(szbuf); put_s("  root  root  ");
        put_s(datebuf); put_s("  ");
        put_s(g_entries[i].name);
        put_c('\n');
    }
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"all",        'a', no_argument},
        {"almost-all", 'A', no_argument},
        {"reverse",    'r', no_argument},
        {"recursive",  'R', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "aArRSt", long_opts, &li)) != -1) {
        switch (opt) {
            case 'a': opt_all = 1; break;
            case 'A': opt_almost_all = 1; break;
            case 'r': opt_reverse = 1; break;
            case 'R': opt_recursive = 1; break;
            case 'S': opt_sort_size = 1; break;
            case 't': opt_sort_time = 1; break;
            default:  return 1;
        }
    }
    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) {
        char cwd[UAOS_CMD_PATH_MAX]; uaos_getcwd(cwd, sizeof(cwd));
        vdir_directory(cwd, 0);
    } else {
        for (int i = 0; i < nfiles; i++) {
            const char *fname = uaos_operand(argc, argv, i);
            if (!fname) continue;
            char path[UAOS_CMD_PATH_MAX]; cmd_make_abs(fname, path, sizeof(path));
            struct uaos_stat st;
            if (uaos_stat(path, &st) != 0) { put_s("vdir: "); put_s(fname); put_line(": No such file"); continue; }
            if (st.is_dir) vdir_directory(path, nfiles > 1);
            else {
                char perm[10];
                perm[0] = '-'; perm[1] = (st.protection & UAOS_FIBF_READ) ? '-' : 'r';
                perm[2] = (st.protection & UAOS_FIBF_WRITE) ? '-' : 'w';
                perm[3] = (st.protection & UAOS_FIBF_EXECUTE) ? '-' : 'x';
                perm[4] = '\0';
                char szbuf[16]; uint_to_dec(st.size, szbuf, sizeof(szbuf));
                char datebuf[20]; cmd_fmt_mtime(st.mtime, datebuf, sizeof(datebuf));
                put_s(perm); put_s("  "); put_s(szbuf); put_s("  root  root  ");
                put_s(datebuf); put_s("  "); put_s(fname); put_c('\n');
            }
        }
    }
    return 0;
}
