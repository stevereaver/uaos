/* copy.c — UAOS x86-64 userspace 'copy' command
 *
 * AmigaDOS C:Copy — copy a file or directory tree.
 *   copy <from> <to> [ALL] [CLONE] [DATES] [COM] [QUIET] [BUFFER <n>]
 */

#include "uaos_cmd.h"
#include "uaos_template.h"

static long copy_one(const char *src, const char *dst,
                     int clone, int com, int quiet, int nopro)
{
    long fd = uaos_open(src, UAOS_O_RDONLY);
    if (fd < 0) {
        if (!quiet) { put_s("Failed to copy: "); put_line(src); }
        return -1;
    }
    long out = uaos_open(dst, UAOS_O_WRONLY | UAOS_O_CREAT | UAOS_O_TRUNC);
    if (out < 0) {
        uaos_close((int)fd);
        if (!quiet) { put_s("Failed to copy: "); put_line(src); }
        return -1;
    }

    char buf[256];
    long total = 0;
    for (;;) {
        long n = uaos_read_file((int)fd, (uint8_t *)buf, sizeof(buf));
        if (n <= 0) break;
        uaos_write_file((int)out, (const uint8_t *)buf, (uint32_t)n);
        total += n;
    }
    uaos_close((int)fd);
    uaos_close((int)out);

    if (clone && !nopro) {
        long prot = uaos_getprotection(src);
        uaos_setprotection(dst, (uint16_t)prot);
        long attrs = uaos_getattrs(src);
        uaos_setattrs(dst, (uint8_t)attrs);
    }
    if (com) {
        char comment[80];
        if (uaos_getcomment(src, comment, sizeof(comment)) == 0)
            uaos_setcomment(dst, comment);
    }
    if (!quiet) {
        char msg[UAOS_CMD_LINE_MAX];
        msg[0] = '\0';
        uaos_strlcat(msg, "Copied ", sizeof(msg));
        char num[12];
        uint_to_dec((uint32_t)total, num, sizeof(num));
        uaos_strlcat(msg, num, sizeof(msg));
        uaos_strlcat(msg, " bytes", sizeof(msg));
        put_line(msg);
    }
    return total;
}

static void copy_dir(const char *src, const char *dst,
                     int clone, int com, int quiet, int nopro, const char *pat)
{
    uaos_mkdir(dst);

    long dd = uaos_opendir(src);
    if (dd < 0) return;

    struct uaos_dirent ent;
    while (uaos_readdir((int)dd, &ent) > 0) {
        if (ent.name[0] == '\0') continue;
        int match = !pat || !pat[0] || cmd_pattern_match(ent.name, pat);

        char ssub[UAOS_CMD_PATH_MAX], dsub[UAOS_CMD_PATH_MAX];
        cmd_join_path(src, ent.name, ssub, sizeof(ssub));
        cmd_join_path(dst, ent.name, dsub, sizeof(dsub));

        if (ent.is_dir) {
            if (!pat || !pat[0]) {
                copy_dir(ssub, dsub, clone, com, quiet, nopro, pat);
            } else if (match) {
                copy_dir(ssub, dsub, clone, com, quiet, nopro, NULL);
            } else {
                copy_dir(ssub, dsub, clone, com, quiet, nopro, pat);
            }
        } else if (match) {
            copy_one(ssub, dsub, clone, com, quiet, nopro);
        }
    }
    uaos_closedir((int)dd);
}

int main(int argc, const char **argv)
{
    char args[UAOS_TMPL_MAX_VAL];
    cmd_build_args(argc, argv, args, sizeof(args));

    UaosTmpl t;
    uaos_tmpl_run("FROM/M,TO/A,ALL/S,QUIET/S,BUF=BUFFER/K/N,CLONE/S,DATES/S,NOPRO/S,COM/S,NOREQ/S",
                  &t, args);
    if (t.error[0]) { put_s("copy: "); put_line(t.error); return 20; }

    const char *to = uaos_tmpl_string(&t, "TO");
    if (!to) {
        put_line("Usage: copy <from> <to> [ALL] [CLONE] [DATES] [COM] [QUIET] [NOPRO] [NOREQ] [BUFFER <n>]");
        return 5;
    }

    int all   = uaos_tmpl_switch(&t, "ALL");
    int clone = uaos_tmpl_switch(&t, "CLONE");
    int com   = uaos_tmpl_switch(&t, "COM");
    int quiet = uaos_tmpl_switch(&t, "QUIET");
    int nopro = uaos_tmpl_switch(&t, "NOPRO");
    /* NOREQ is accepted but has no effect in userspace (no requesters). */
    (void)uaos_tmpl_switch(&t, "NOREQ");

    char abs_dst[UAOS_CMD_PATH_MAX];
    cmd_make_abs(to, abs_dst, sizeof(abs_dst));

    int n = uaos_tmpl_count(&t, "FROM");
    if (n == 0) {
        put_line("Usage: copy <from> <to> [ALL] [CLONE] [DATES] [COM] [QUIET] [NOPRO] [NOREQ] [BUFFER <n>]");
        return 5;
    }

    for (int i = 0; i < n; i++) {
        const char *from = uaos_tmpl_multi(&t, "FROM", i);
        if (!from) continue;

        char abs_src[UAOS_CMD_PATH_MAX], src_pat[UAOS_CMD_PATH_MAX];
        cmd_split_path_pat(from, abs_src, src_pat);

        if (src_pat[0]) {
            uaos_mkdir(abs_dst);
            long dd = uaos_opendir(abs_src);
            if (dd >= 0) {
                struct uaos_dirent ent;
                while (uaos_readdir((int)dd, &ent) > 0) {
                    if (ent.name[0] == '\0') continue;
                    if (cmd_pattern_match(ent.name, src_pat)) {
                        char ssub[UAOS_CMD_PATH_MAX], dsub[UAOS_CMD_PATH_MAX];
                        cmd_join_path(abs_src, ent.name, ssub, sizeof(ssub));
                        cmd_join_path(abs_dst, ent.name, dsub, sizeof(dsub));
                        if (ent.is_dir && all) {
                            copy_dir(ssub, dsub, clone, com, quiet, nopro, src_pat);
                        } else if (!ent.is_dir) {
                            copy_one(ssub, dsub, clone, com, quiet, nopro);
                        }
                    }
                }
                uaos_closedir((int)dd);
            }
        } else {
            struct uaos_stat st;
            int is_dir = (uaos_stat(abs_src, &st) == 0 && st.is_dir);
            if (is_dir && all) {
                copy_dir(abs_src, abs_dst, clone, com, quiet, nopro, NULL);
            } else {
                copy_one(abs_src, abs_dst, clone, com, quiet, nopro);
            }
        }
    }
    return 0;
}
