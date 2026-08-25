/* filenote.c — UAOS x86-64 userspace 'filenote' command
 *
 * AmigaDOS C:Filenote — set or show a file's comment string.
 *   filenote <file> [comment] [ALL] [QUIET]
 *
 * Template: FILE/A,COMMENT,ALL/S,QUIET/S
 */

#include "uaos_cmd.h"
#include "uaos_template.h"

static void filenote_one(const char *path, const char *comment, int quiet)
{
    if (comment && comment[0]) {
        if (uaos_setcomment(path, comment) == 0) {
            if (!quiet) { put_s("Filenote set: "); put_line(path); }
        } else {
            if (!quiet) { put_s("Failed to set filenote: "); put_line(path); }
        }
    } else {
        char note[80];
        if (uaos_getcomment(path, note, sizeof(note)) == 0 && note[0]) {
            if (!quiet) {
                put_s("Filenote for ");
                put_s(path);
                put_s(": ");
                put_line(note);
            }
        } else {
            if (!quiet) {
                put_s("No filenote for ");
                put_line(path);
            }
        }
    }
}

static void filenote_dir(const char *path, const char *comment,
                         int quiet, const char *pat)
{
    if (!pat || !pat[0]) filenote_one(path, comment, quiet);

    long dd = uaos_opendir(path);
    if (dd < 0) return;
    struct uaos_dirent ent;
    while (uaos_readdir((int)dd, &ent) > 0) {
        if (ent.name[0] == '\0') continue;
        if (pat && pat[0] && !cmd_pattern_match(ent.name, pat)) continue;

        char sub[UAOS_CMD_PATH_MAX];
        cmd_join_path(path, ent.name, sub, sizeof(sub));
        filenote_one(sub, comment, quiet);
        if (ent.is_dir) filenote_dir(sub, comment, quiet, pat);
    }
    uaos_closedir((int)dd);
}

int main(int argc, const char **argv)
{
    char args[UAOS_TMPL_MAX_VAL];
    cmd_build_args(argc, argv, args, sizeof(args));

    UaosTmpl t;
    uaos_tmpl_run("FILE/A,COMMENT/F,ALL/S,QUIET/S", &t, args);
    if (t.error[0]) { put_s("filenote: "); put_line(t.error); return 20; }

    const char *file_arg = uaos_tmpl_string(&t, "FILE");
    if (!file_arg) {
        put_line("Usage: filenote <file> [comment] [ALL] [QUIET]");
        put_line("  Without comment: show current filenote.");
        put_line("  With comment:    set filenote.");
        put_line("  ALL:             apply to all files in directory.");
        return 5;
    }

    int all   = uaos_tmpl_switch(&t, "ALL");
    int quiet = uaos_tmpl_switch(&t, "QUIET");
    const char *comment = uaos_tmpl_string(&t, "COMMENT");

    char path[UAOS_CMD_PATH_MAX], pat[UAOS_CMD_PATH_MAX];
    cmd_split_path_pat(file_arg, path, pat);

    if (all || pat[0]) {
        filenote_dir(path, comment, quiet, pat);
    } else {
        filenote_one(path, comment, quiet);
    }
    return 0;
}
