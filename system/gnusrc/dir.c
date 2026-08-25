/* dir.c — GNU coreutils 'dir' for UAOS gnu: layer
 *
 * Like 'ls' but defaults to -C (column) format and no color.
 *   dir [OPTION]... [FILE]...
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

/* dir is equivalent to ls -C -p; we delegate to ls logic. */

#define LS_MAX_ENTRIES 512

static int opt_long = 0;
static int opt_all = 0;
static int opt_almost_all = 0;
static int opt_one = 0;
static int opt_reverse = 0;
static int opt_sort_size = 0;
static int opt_sort_time = 0;
static int opt_recursive = 0;
static int opt_directory = 0;
static int opt_classify = 0;
static int opt_slash = 1; /* dir defaults to -p */

typedef struct {
    char name[32];
    uint32_t size;
    uint8_t  is_dir;
    uint16_t protection;
    uint32_t mtime;
} DirEntry;

static DirEntry g_entries[LS_MAX_ENTRIES];
static int g_entry_count = 0;

static int dir_cmp(const DirEntry *a, const DirEntry *b)
{
    int r = uaos_strcmp(a->name, b->name);
    if (opt_reverse) r = -r;
    return r;
}

static void dir_sort(void)
{
    for (int i = 0; i < g_entry_count - 1; i++)
        for (int j = 0; j < g_entry_count - 1 - i; j++)
            if (dir_cmp(&g_entries[j], &g_entries[j + 1]) > 0) {
                DirEntry tmp = g_entries[j];
                g_entries[j] = g_entries[j + 1];
                g_entries[j + 1] = tmp;
            }
}

static void dir_directory(const char *path)
{
    long dd = uaos_opendir(path);
    if (dd < 0) { put_s("dir: cannot access '"); put_s(path); put_line("'"); return; }
    g_entry_count = 0;
    struct uaos_dirent ent;
    while (uaos_readdir((int)dd, &ent) > 0 && g_entry_count < LS_MAX_ENTRIES) {
        if (ent.name[0] == '.') {
            if (!opt_all && !opt_almost_all) continue;
            if (opt_almost_all && (ent.name[1] == '\0' || (ent.name[1] == '.' && ent.name[2] == '\0'))) continue;
        }
        uaos_strcpy(g_entries[g_entry_count].name, ent.name);
        g_entries[g_entry_count].size = ent.size;
        g_entries[g_entry_count].is_dir = ent.is_dir;
        g_entries[g_entry_count].protection = ent.protection;
        g_entries[g_entry_count].mtime = ent.mtime;
        g_entry_count++;
    }
    uaos_closedir((int)dd);
    dir_sort();
    for (int i = 0; i < g_entry_count; i++) {
        put_s(g_entries[i].name);
        if (opt_slash && g_entries[i].is_dir) put_c('/');
        put_s("  ");
    }
    if (g_entry_count > 0) put_c('\n');
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
    while ((opt = uaos_getopt_long(argc, argv, "laArR", long_opts, &li)) != -1) {
        switch (opt) {
            case 'l': opt_long = 1; break;
            case 'a': opt_all = 1; break;
            case 'A': opt_almost_all = 1; break;
            case 'r': opt_reverse = 1; break;
            case 'R': opt_recursive = 1; break;
            default:  return 1;
        }
    }
    int nfiles = uaos_operands_count(argc);
    if (nfiles == 0) {
        char cwd[UAOS_CMD_PATH_MAX];
        uaos_getcwd(cwd, sizeof(cwd));
        dir_directory(cwd);
    } else {
        for (int i = 0; i < nfiles; i++) {
            const char *fname = uaos_operand(argc, argv, i);
            if (!fname) continue;
            char path[UAOS_CMD_PATH_MAX];
            cmd_make_abs(fname, path, sizeof(path));
            struct uaos_stat st;
            if (uaos_stat(path, &st) != 0) { put_s("dir: "); put_s(fname); put_line(": No such file"); continue; }
            if (st.is_dir) dir_directory(path);
            else { put_s(fname); put_c('\n'); }
        }
    }
    return 0;
}
