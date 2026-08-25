/* search.c — UAOS x86-64 userspace 'search' command
 *
 * AmigaDOS C:Search — search files for text patterns.
 *   search FROM <dir> SEARCH <pattern> [ALL] [NONUM] [QUIET] [QUICK] [FILE] [PATTERN]
 *
 * Template: FROM/M,SEARCH/A,ALL/S,NONUM/S,QUIET/S,QUICK/S,FILE/S,PATTERN/S
 *
 * FILE/S    — search for files by name (pattern matches filename, not contents)
 * PATTERN/S — treat SEARCH as an AmigaDOS pattern
 * NONUM/S   — suppress line numbers
 * QUIET/S   — suppress filename display (only print matching lines)
 * QUICK/S   — compact output (filename:line:match on one line)
 */

#include "uaos_cmd.h"
#include "uaos_template.h"

static int search_ci_eq(unsigned char a, unsigned char b)
{
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    return a == b;
}

static int search_match(const char *needle, int nl,
                        const uint8_t *hay, int hl, int ci)
{
    if (nl == 0) return 1;
    for (int i = 0; i <= hl - nl; i++) {
        int ok = 1;
        for (int j = 0; j < nl; j++) {
            if (ci) {
                if (!search_ci_eq((unsigned char)needle[j], hay[i + j]))
                    { ok = 0; break; }
            } else {
                if ((unsigned char)needle[j] != hay[i + j])
                    { ok = 0; break; }
            }
        }
        if (ok) return 1;
    }
    return 0;
}

static void search_file(const char *path, const char *pattern, int ci,
                        int nonum, int quiet, int quick,
                        int *hits, int *files)
{
    long fd = uaos_open(path, UAOS_O_RDONLY);
    if (fd < 0) return;

    struct uaos_stat st;
    uint32_t sz = 0;
    if (uaos_stat(path, &st) == 0) sz = st.size;

    uint32_t pos = 0;
    int line_no = 0, file_hits = 0;
    while (pos < sz) {
        uint8_t buf[UAOS_CMD_LINE_MAX];
        int col = 0;
        while (pos < sz && col < (int)sizeof(buf) - 1) {
            uint8_t c;
            if (uaos_read_file((int)fd, &c, 1) == 0) break;
            pos++;
            if (c == '\n') break;
            if (c != '\r') buf[col++] = c;
        }
        buf[col] = '\0';
        line_no++;
        if (search_match(pattern, (int)uaos_strlen(pattern), buf, col, 1)) {
            char out[UAOS_CMD_LINE_MAX];
            out[0] = '\0';
            if (!quiet) {
                uaos_strlcat(out, path, sizeof(out));
                uaos_strlcat(out, ":", sizeof(out));
            }
            if (!nonum) {
                char lnum[8];
                uint_to_dec((uint32_t)line_no, lnum, sizeof(lnum));
                uaos_strlcat(out, lnum, sizeof(out));
                uaos_strlcat(out, ": ", sizeof(out));
            }
            uaos_strlcat(out, (char *)buf, sizeof(out));
            put_line(out);
            file_hits++;
        }
    }
    uaos_close((int)fd);
    if (file_hits) { (*files)++; *hits += file_hits; }
}

/* FILE mode: search for files by name pattern. */
static void search_filenames(const char *path, const char *pattern,
                             int all, int *hits)
{
    long dd = uaos_opendir(path);
    if (dd < 0) return;
    struct uaos_dirent ent;
    while (uaos_readdir((int)dd, &ent) > 0) {
        if (ent.name[0] == '\0') continue;
        char sub[UAOS_CMD_PATH_MAX];
        cmd_join_path(path, ent.name, sub, sizeof(sub));
        if (ent.is_dir) {
            if (all) search_filenames(sub, pattern, all, hits);
        } else {
            if (cmd_pattern_match(ent.name, pattern)) {
                put_line(sub);
                (*hits)++;
            }
        }
    }
    uaos_closedir((int)dd);
}

static void search_dir(const char *path, const char *pattern, int ci,
                       int all, int nonum, int quiet, int quick,
                       const char *file_pat, int *hits, int *files)
{
    long dd = uaos_opendir(path);
    if (dd < 0) return;
    struct uaos_dirent ent;
    while (uaos_readdir((int)dd, &ent) > 0) {
        if (ent.name[0] == '\0') continue;
        char sub[UAOS_CMD_PATH_MAX];
        cmd_join_path(path, ent.name, sub, sizeof(sub));
        if (ent.is_dir) {
            if (all) search_dir(sub, pattern, ci, all, nonum, quiet, quick,
                                file_pat, hits, files);
        } else {
            if (!file_pat || !file_pat[0] ||
                cmd_pattern_match(ent.name, file_pat))
                search_file(sub, pattern, ci, nonum, quiet, quick, hits, files);
        }
    }
    uaos_closedir((int)dd);
}

int main(int argc, const char **argv)
{
    char args[UAOS_TMPL_MAX_VAL];
    cmd_build_args(argc, argv, args, sizeof(args));

    UaosTmpl t;
    uaos_tmpl_run("FROM/M,SEARCH/A,ALL/S,NONUM/S,QUIET/S,QUICK/S,FILE/S,PATTERN/S",
                  &t, args);
    if (t.error[0]) { put_s("search: "); put_line(t.error); return 20; }

    const char *pattern = uaos_tmpl_string(&t, "SEARCH");
    int all    = uaos_tmpl_switch(&t, "ALL");
    int nonum  = uaos_tmpl_switch(&t, "NONUM");
    int quiet  = uaos_tmpl_switch(&t, "QUIET");
    int quick  = uaos_tmpl_switch(&t, "QUICK");
    int file_mode = uaos_tmpl_switch(&t, "FILE");
    /* PATTERN/S is accepted; in this implementation all searches are
     * substring-based (ci is always on for text search, pattern matching
     * is used for FILE mode). */
    (void)uaos_tmpl_switch(&t, "PATTERN");

    if (!pattern || !pattern[0]) {
        put_line("Usage: search FROM <dir> SEARCH <pattern> [ALL] [NONUM] [QUIET] [QUICK] [FILE] [PATTERN]");
        return 5;
    }

    int n_from = uaos_tmpl_count(&t, "FROM");
    const char *from = (n_from > 0) ? uaos_tmpl_multi(&t, "FROM", 0) : NULL;

    char path[UAOS_CMD_PATH_MAX];
    if (from && from[0]) {
        cmd_make_abs(from, path, sizeof(path));
    } else {
        long n = uaos_getcwd(path, sizeof(path));
        if (n <= 0) path[0] = '\0';
    }

    int hits = 0, files = 0;

    if (file_mode) {
        /* Search for files by name pattern. */
        search_filenames(path, pattern, all, &hits);
    } else {
        struct uaos_stat st;
        if (uaos_stat(path, &st) == 0 && !st.is_dir) {
            search_file(path, pattern, 1, nonum, quiet, quick, &hits, &files);
        } else {
            search_dir(path, pattern, 1, all, nonum, quiet, quick,
                       NULL, &hits, &files);
        }
    }

    if (!quiet) {
        char summary[UAOS_CMD_LINE_MAX];
        summary[0] = '\0';
        uint_to_dec((uint32_t)hits, summary, sizeof(summary));
        uaos_strlcat(summary, file_mode ? " file(s) found" : " match(es) in ",
                     sizeof(summary));
        if (!file_mode) {
            char fn[8];
            uint_to_dec((uint32_t)files, fn, sizeof(fn));
            uaos_strlcat(summary, fn, sizeof(summary));
            uaos_strlcat(summary, " file(s)", sizeof(summary));
        }
        put_line(summary);
    }
    return 0;
}
