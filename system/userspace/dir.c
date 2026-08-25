/* dir.c — UAOS x86-64 userspace 'dir' command
 *
 * AmigaDOS C:Dir — list a directory.
 *   dir [dir] [ALL] [DATES] [INTER] [KEYS] [OPT A|D]
 */

#include "uaos_cmd.h"
#include "uaos_template.h"

#define DIR_MAX_ENTRIES 256

struct dir_entry {
    char     name[32];
    uint32_t size;
    uint8_t  is_dir;
    uint32_t mtime;
};

static int entry_cmp_name(const struct dir_entry *a, const struct dir_entry *b)
{
    const char *pa = a->name, *pb = b->name;
    while (*pa && *pb) {
        char ca = *pa; if (ca >= 'A' && ca <= 'Z') ca += 32;
        char cb = *pb; if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        pa++; pb++;
    }
    return (unsigned char)*pa - (unsigned char)*pb;
}

static int entry_cmp_dirfirst(const struct dir_entry *a, const struct dir_entry *b)
{
    if (a->is_dir && !b->is_dir) return -1;
    if (!a->is_dir && b->is_dir) return 1;
    return entry_cmp_name(a, b);
}

static void print_entry(const struct dir_entry *e, int dates)
{
    char line[UAOS_CMD_LINE_MAX];
    line[0] = '\0';
    if (e->is_dir) {
        uaos_strlcat(line, "  ", sizeof(line));
        uaos_strlcat(line, e->name, sizeof(line));
        uaos_strlcat(line, "  (dir)", sizeof(line));
    } else {
        char sz[12];
        uint_to_dec(e->size, sz, sizeof(sz));
        uaos_strlcat(line, "  ", sizeof(line));
        uaos_strlcat(line, e->name, sizeof(line));
        uaos_strlcat(line, "  ", sizeof(line));
        uaos_strlcat(line, sz, sizeof(line));
        uaos_strlcat(line, " bytes", sizeof(line));
    }
    if (dates) {
        char dstr[24];
        cmd_fmt_mtime(e->mtime, dstr, sizeof(dstr));
        uaos_strlcat(line, "  ", sizeof(line));
        uaos_strlcat(line, dstr, sizeof(line));
    }
    put_line(line);
}

static int prompt_yn(const char *msg)
{
    put_s(msg);
    put_s(" (y/n)? ");
    long k = uaos_readkey();
    put_c('\n');
    return (k == 'y' || k == 'Y');
}

static void dir_list(const char *path, const char *pat,
                     int all, int dates, int inter, int keys,
                     int opt_alpha, int opt_dirfirst,
                     int dirs_only, int files_only, int *lines)
{
    long dd = uaos_opendir(path);
    if (dd < 0) return;

    static struct dir_entry ents[DIR_MAX_ENTRIES];
    int count = 0;
    struct uaos_dirent de;
    while (uaos_readdir((int)dd, &de) > 0 && count < DIR_MAX_ENTRIES) {
        if (de.name[0] == '\0') continue;
        if (!pat[0] || cmd_pattern_match(de.name, pat)) {
            uaos_strcpy(ents[count].name, de.name);
            ents[count].size   = de.size;
            ents[count].is_dir = de.is_dir;
            ents[count].mtime  = de.mtime;
            count++;
        }
    }
    uaos_closedir((int)dd);

    if (opt_dirfirst) {
        for (int i = 0; i < count - 1; i++)
            for (int j = 0; j < count - 1 - i; j++)
                if (entry_cmp_dirfirst(&ents[j], &ents[j + 1]) > 0) {
                    struct dir_entry tmp = ents[j]; ents[j] = ents[j + 1]; ents[j + 1] = tmp;
                }
    } else if (opt_alpha) {
        for (int i = 0; i < count - 1; i++)
            for (int j = 0; j < count - 1 - i; j++)
                if (entry_cmp_name(&ents[j], &ents[j + 1]) > 0) {
                    struct dir_entry tmp = ents[j]; ents[j] = ents[j + 1]; ents[j + 1] = tmp;
                }
    }

    for (int i = 0; i < count; i++) {
        /* Apply DIRS/FILES filters. */
        if (dirs_only && !ents[i].is_dir) continue;
        if (files_only && ents[i].is_dir) continue;

        if (inter) {
            char prompt[UAOS_CMD_LINE_MAX];
            prompt[0] = '\0';
            uaos_strlcat(prompt, "List ", sizeof(prompt));
            uaos_strlcat(prompt, ents[i].name, sizeof(prompt));
            if (!prompt_yn(prompt)) continue;
        }
        print_entry(&ents[i], dates);
        (*lines)++;
        if (keys && (*lines) % 20 == 0) {
            put_line("-- Press any key --");
            uaos_readkey();
        }
        if (all && ents[i].is_dir) {
            char sub[UAOS_CMD_PATH_MAX];
            cmd_join_path(path, ents[i].name, sub, sizeof(sub));
            dir_list(sub, pat, all, dates, inter, keys, opt_alpha, opt_dirfirst,
                     dirs_only, files_only, lines);
        }
    }
}

int main(int argc, const char **argv)
{
    char args[UAOS_TMPL_MAX_VAL];
    cmd_build_args(argc, argv, args, sizeof(args));

    UaosTmpl t;
    uaos_tmpl_run("DIR,OPT/K,ALL/S,DIRS/S,FILES/S,INTER/S,DATES/S,KEYS/S", &t, args);
    if (t.error[0]) { put_s("dir: "); put_line(t.error); return 20; }

    int all = uaos_tmpl_switch(&t, "ALL");
    int dates = uaos_tmpl_switch(&t, "DATES");
    int inter = uaos_tmpl_switch(&t, "INTER");
    int keys  = uaos_tmpl_switch(&t, "KEYS");
    int dirs_only  = uaos_tmpl_switch(&t, "DIRS");
    int files_only = uaos_tmpl_switch(&t, "FILES");
    int opt_alpha = 0, opt_dirfirst = 0;
    const char *opt_str = uaos_tmpl_string(&t, "OPT");
    if (opt_str) {
        for (int i = 0; opt_str[i]; i++) {
            char c = opt_str[i]; if (c >= 'A' && c <= 'Z') c += 32;
            if (c == 'a') opt_alpha = 1;
            if (c == 'd') opt_dirfirst = 1;
        }
    }

    char path[UAOS_CMD_PATH_MAX], pat[UAOS_CMD_PATH_MAX];
    const char *dir = uaos_tmpl_string(&t, "DIR");
    if (dir) {
        cmd_split_path_pat(dir, path, pat);
    } else {
        long n = uaos_getcwd(path, sizeof(path));
        if (n <= 0) path[0] = '\0';
        pat[0] = '\0';
    }

    char hdr[UAOS_CMD_LINE_MAX];
    hdr[0] = '\0';
    uaos_strlcat(hdr, "Directory of ", sizeof(hdr));
    uaos_strlcat(hdr, path, sizeof(hdr));
    if (pat[0]) {
        uaos_strlcat(hdr, "  (pattern: ", sizeof(hdr));
        uaos_strlcat(hdr, pat, sizeof(hdr));
        uaos_strlcat(hdr, ")", sizeof(hdr));
    }
    put_line(hdr);
    put_line("");

    long dd = uaos_opendir(path);
    if (dd < 0) {
        put_line("  (empty or not found)");
        put_line("");
        return 0;
    }
    /* Reopen for dir_list (it opens its own handle). */
    uaos_closedir((int)dd);

    int lines = 2;
    dir_list(path, pat, all, dates, inter, keys, opt_alpha, opt_dirfirst,
             dirs_only, files_only, &lines);

    put_line("");

    /* Summary: count + bytes used by matching files + free space. */
    uint32_t bytes_used = 0;
    int count = 0;
    long dd2 = uaos_opendir(path);
    if (dd2 >= 0) {
        struct uaos_dirent de;
        while (uaos_readdir((int)dd2, &de) > 0) {
            if (!pat[0] || cmd_pattern_match(de.name, pat)) {
                count++;
                if (!de.is_dir) bytes_used += de.size;
            }
        }
        uaos_closedir((int)dd2);
    }

    uint32_t total = 0, used = 0;
    uaos_getvolumeinfo(path, &total, &used);
    uint32_t free_bytes = (total > used) ? (total - used) : 0;

    char summary[UAOS_CMD_LINE_MAX];
    summary[0] = '\0';
    char cn[8];
    uint_to_dec((uint32_t)count, cn, sizeof(cn));
    uaos_strlcat(summary, cn, sizeof(summary));
    uaos_strlcat(summary, " item(s)  ", sizeof(summary));
    char bu[12];
    uint_to_dec(bytes_used, bu, sizeof(bu));
    uaos_strlcat(summary, bu, sizeof(summary));
    uaos_strlcat(summary, " bytes used  ", sizeof(summary));
    char bf[12];
    uint_to_dec(free_bytes, bf, sizeof(bf));
    uaos_strlcat(summary, bf, sizeof(summary));
    uaos_strlcat(summary, " bytes free", sizeof(summary));
    put_line(summary);
    return 0;
}
