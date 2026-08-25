/* list.c — UAOS x86-64 userspace 'list' command
 *
 * AmigaDOS C:List — detailed directory listing.
 *   list [dir] [ALL] [DATES] [INTER] [KEYS] [NOHEAD] [LFORMAT "fmt"]
 *
 * LFORMAT specifiers: %N name  %S size  %T type  %P protection
 *                     %C comment  %D date  %L line number
 */

#include "uaos_cmd.h"
#include "uaos_template.h"

#define LIST_MAX_ENTRIES 256

struct list_entry {
    char     name[32];
    uint32_t size;
    uint8_t  is_dir;
    uint8_t  attrs;
    uint16_t protection;
    uint32_t mtime;
    char     comment[64];
};

static int le_cmp_name(const struct list_entry *a, const struct list_entry *b)
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

static void le_prot_str(char *prot, uint8_t attrs)
{
    uaos_strcpy(prot, "rwed----");
    if (attrs & UAOS_ATTR_READONLY) { prot[0] = '-'; prot[1] = '-'; }
    if (attrs & UAOS_ATTR_HIDDEN)   { prot[4] = 'h'; }
}

static void format_line(char *out, int max, const char *fmt,
                        const struct list_entry *e, int line_no)
{
    out[0] = '\0';
    const char *p = fmt;
    while (*p && (int)uaos_strlen(out) < max - 1) {
        if (*p == '%' && p[1]) {
            char spec = p[1];
            p += 2;
            switch (spec) {
                case 'N': case 'n': uaos_strlcat(out, e->name, max); break;
                case 'S': case 's': {
                    char sz[12];
                    if (e->is_dir) uaos_strcpy(sz, "<dir>");
                    else uint_to_dec(e->size, sz, sizeof(sz));
                    uaos_strlcat(out, sz, max);
                    break;
                }
                case 'T': case 't':
                    uaos_strlcat(out, e->is_dir ? "Dir" : "File", max);
                    break;
                case 'P': case 'p': {
                    char prot[9];
                    le_prot_str(prot, e->attrs);
                    uaos_strlcat(out, prot, max);
                    break;
                }
                case 'C': case 'c':
                    uaos_strlcat(out, e->comment, max);
                    break;
                case 'D': case 'd': {
                    char dstr[24];
                    cmd_fmt_mtime(e->mtime, dstr, sizeof(dstr));
                    uaos_strlcat(out, dstr, max);
                    break;
                }
                case 'L': case 'l': {
                    char lnum[8];
                    uint_to_dec((uint32_t)line_no, lnum, sizeof(lnum));
                    uaos_strlcat(out, lnum, max);
                    break;
                }
                default: break;
            }
        } else {
            int li = (int)uaos_strlen(out);
            if (li < max - 1) { out[li] = *p; out[li + 1] = '\0'; }
            p++;
        }
    }
}

static void print_entry(const struct list_entry *e, int dates,
                        int lformat, int quick, const char *fmt,
                        int line_no, long out_fd)
{
    char line[UAOS_CMD_LINE_MAX];
    if (lformat && fmt[0]) {
        format_line(line, sizeof(line), fmt, e, line_no);
    } else if (quick) {
        /* QUICK: just the name. */
        uaos_strcpy(line, e->name);
    } else {
        line[0] = '\0';
        char prot[9];
        le_prot_str(prot, e->attrs);
        char sz[12];
        if (e->is_dir) uaos_strcpy(sz, "   <dir>");
        else uint_to_dec(e->size, sz, sizeof(sz));
        uaos_strlcat(line, "  ", sizeof(line));
        uaos_strlcat(line, prot, sizeof(line));
        uaos_strlcat(line, "  ", sizeof(line));
        uaos_strlcat(line, sz, sizeof(line));
        uaos_strlcat(line, "  ", sizeof(line));
        uaos_strlcat(line, e->name, sizeof(line));
        if (dates) {
            char dstr[24];
            cmd_fmt_mtime(e->mtime, dstr, sizeof(dstr));
            uaos_strlcat(line, "  ", sizeof(line));
            uaos_strlcat(line, dstr, sizeof(line));
        }
        if (e->comment[0]) {
            uaos_strlcat(line, "  (", sizeof(line));
            uaos_strlcat(line, e->comment, sizeof(line));
            uaos_strlcat(line, ")", sizeof(line));
        }
    }
    if (out_fd >= 0) {
        uaos_write_file((int)out_fd, (const uint8_t *)line, (uint32_t)uaos_strlen(line));
        uaos_write_file((int)out_fd, (const uint8_t *)"\n", 1);
    } else {
        put_line(line);
    }
}

static int prompt_yn(const char *msg)
{
    put_s(msg);
    put_s(" (y/n)? ");
    long k = uaos_readkey();
    put_c('\n');
    return (k == 'y' || k == 'Y');
}

static void list_dir(const char *path, int all, int dates, int inter, int keys,
                     int lformat, int quick, int dirs_only, int files_only,
                     const char *fmt, long out_fd,
                     int *total_lines, int *total_files,
                     int *total_dirs, uint32_t *total_size)
{
    long dd = uaos_opendir(path);
    if (dd < 0) return;

    static struct list_entry ents[LIST_MAX_ENTRIES];
    int count = 0;
    struct uaos_dirent de;
    while (uaos_readdir((int)dd, &de) > 0 && count < LIST_MAX_ENTRIES) {
        if (de.name[0] == '\0') continue;
        uaos_strcpy(ents[count].name, de.name);
        ents[count].size       = de.size;
        ents[count].is_dir     = de.is_dir;
        ents[count].attrs      = de.attrs;
        ents[count].protection = de.protection;
        ents[count].mtime      = de.mtime;
        ents[count].comment[0] = '\0';
        char sub[UAOS_CMD_PATH_MAX];
        cmd_join_path(path, de.name, sub, sizeof(sub));
        uaos_getcomment(sub, ents[count].comment, sizeof(ents[count].comment));
        count++;
    }
    uaos_closedir((int)dd);

    for (int i = 0; i < count - 1; i++)
        for (int j = 0; j < count - 1 - i; j++)
            if (le_cmp_name(&ents[j], &ents[j + 1]) > 0) {
                struct list_entry tmp = ents[j]; ents[j] = ents[j + 1]; ents[j + 1] = tmp;
            }

    for (int i = 0; i < count; i++) {
        /* Apply DIRS/FILES filters. */
        if (dirs_only && !ents[i].is_dir) continue;
        if (files_only && ents[i].is_dir) continue;

        if (inter) {
            char pr[UAOS_CMD_LINE_MAX];
            pr[0] = '\0';
            uaos_strlcat(pr, "List ", sizeof(pr));
            uaos_strlcat(pr, ents[i].name, sizeof(pr));
            if (!prompt_yn(pr)) continue;
        }
        (*total_files)++;
        if (ents[i].is_dir) (*total_dirs)++;
        else *total_size += ents[i].size;
        print_entry(&ents[i], dates, lformat, quick, fmt, *total_files, out_fd);
        (*total_lines)++;
        if (keys && (*total_lines) % 20 == 0 && out_fd < 0) {
            put_line("-- Press any key --");
            uaos_readkey();
        }
        if (all && ents[i].is_dir) {
            char sub[UAOS_CMD_PATH_MAX];
            cmd_join_path(path, ents[i].name, sub, sizeof(sub));
            list_dir(sub, all, dates, inter, keys, lformat, quick,
                     dirs_only, files_only, fmt, out_fd,
                     total_lines, total_files, total_dirs, total_size);
        }
    }
}

int main(int argc, const char **argv)
{
    char args[UAOS_TMPL_MAX_VAL];
    cmd_build_args(argc, argv, args, sizeof(args));

    UaosTmpl t;
    uaos_tmpl_run("DIR/M,P=PAT/K,KEYS/S,DATES/S,NODATES/S,TO/K,SUB/K,"
                  "QUICK/S,NOHEAD/S,FILES/S,DIRS/S,LFORMAT/K,ALL/S",
                  &t, args);
    if (t.error[0]) { put_s("list: "); put_line(t.error); return 20; }

    int all    = uaos_tmpl_switch(&t, "ALL");
    int dates  = uaos_tmpl_switch(&t, "DATES");
    int inter  = 0; /* INTER not in standard LIST template */
    int keys   = uaos_tmpl_switch(&t, "KEYS");
    int nohead = uaos_tmpl_switch(&t, "NOHEAD");
    int quick  = uaos_tmpl_switch(&t, "QUICK");
    int dirs_only  = uaos_tmpl_switch(&t, "DIRS");
    int files_only = uaos_tmpl_switch(&t, "FILES");
    int lformat = uaos_tmpl_string(&t, "LFORMAT") != NULL;
    const char *fmt = uaos_tmpl_string(&t, "LFORMAT");
    if (!fmt) fmt = "";

    /* TO output file. */
    long out_fd = -1;
    const char *to_file = uaos_tmpl_string(&t, "TO");
    if (to_file && to_file[0]) {
        char abs_to[UAOS_CMD_PATH_MAX];
        cmd_make_abs(to_file, abs_to, sizeof(abs_to));
        out_fd = uaos_open(abs_to, UAOS_O_WRONLY | UAOS_O_CREAT | UAOS_O_TRUNC);
        if (out_fd < 0) { put_s("list: cannot open: "); put_line(to_file); }
    }

    /* Get directory argument (first FROM value). */
    int n_dirs = uaos_tmpl_count(&t, "DIR");
    const char *dir_arg = (n_dirs > 0) ? uaos_tmpl_multi(&t, "DIR", 0) : NULL;

    /* Pattern filter via P=PAT. */
    const char *pat = uaos_tmpl_string(&t, "PAT");

    char path[UAOS_CMD_PATH_MAX];
    if (dir_arg && dir_arg[0]) {
        cmd_make_abs(dir_arg, path, sizeof(path));
    } else {
        long n = uaos_getcwd(path, sizeof(path));
        if (n <= 0) path[0] = '\0';
    }

    if (!nohead && out_fd < 0) {
        char hdr[UAOS_CMD_LINE_MAX];
        hdr[0] = '\0';
        uaos_strlcat(hdr, "Directory of ", sizeof(hdr));
        uaos_strlcat(hdr, path, sizeof(hdr));
        put_line(hdr);
        put_line("");
    }

    long dd = uaos_opendir(path);
    if (dd < 0) {
        if (out_fd < 0) {
            put_line("  (empty or not found)");
            put_line("");
        }
        return 0;
    }
    uaos_closedir((int)dd);

    int lines = (nohead || out_fd >= 0) ? 0 : 2;
    int files = 0, dirs = 0;
    uint32_t total_size = 0;
    list_dir(path, all, dates, inter, keys, lformat, quick,
             dirs_only, files_only, fmt, out_fd,
             &lines, &files, &dirs, &total_size);

    if (out_fd < 0) {
        put_line("");
        char summary[UAOS_CMD_LINE_MAX];
        summary[0] = '\0';
        char cn[8];
        uint_to_dec((uint32_t)(files + dirs), cn, sizeof(cn));
        uaos_strlcat(summary, cn, sizeof(summary));
        uaos_strlcat(summary, " entries (", sizeof(summary));
        uint_to_dec((uint32_t)files, cn, sizeof(cn));
        uaos_strlcat(summary, cn, sizeof(summary));
        uaos_strlcat(summary, " files, ", sizeof(summary));
        uint_to_dec((uint32_t)dirs, cn, sizeof(cn));
        uaos_strlcat(summary, cn, sizeof(summary));
        uaos_strlcat(summary, " dirs)", sizeof(summary));
        put_line(summary);

        char ts[UAOS_CMD_LINE_MAX];
        ts[0] = '\0';
        uaos_strlcat(ts, "Total bytes: ", sizeof(ts));
        uint_to_dec(total_size, cn, sizeof(cn));
        uaos_strlcat(ts, cn, sizeof(ts));
        put_line(ts);
    } else {
        uaos_close((int)out_fd);
    }
    return 0;
}
