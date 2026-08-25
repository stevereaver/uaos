/* grep.c — UAOS x86-64 userspace 'grep' command
 *
 * AmigaDOS C:Grep — search file contents for a pattern.
 *   grep <pattern> [file] [CI]
 */

#include "uaos_cmd.h"
#include "uaos_template.h"

static int grep_ci_eq(unsigned char a, unsigned char b)
{
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    return a == b;
}

static int grep_match(const char *needle, int nl,
                      const uint8_t *hay, int hl, int ci)
{
    if (nl == 0) return 1;
    for (int i = 0; i <= hl - nl; i++) {
        int ok = 1;
        for (int j = 0; j < nl; j++) {
            if (ci) {
                if (!grep_ci_eq((unsigned char)needle[j], hay[i + j]))
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

int main(int argc, const char **argv)
{
    char args[UAOS_TMPL_MAX_VAL];
    cmd_build_args(argc, argv, args, sizeof(args));

    UaosTmpl t;
    uaos_tmpl_run("PATTERN/A,FILE,CI/S", &t, args);
    if (t.error[0]) { put_s("grep: "); put_line(t.error); return 20; }

    const char *pattern = uaos_tmpl_string(&t, "PATTERN");
    const char *file_arg = uaos_tmpl_string(&t, "FILE");
    int ci = uaos_tmpl_switch(&t, "CI");

    /* Legacy -i flag support. */
    if (!ci && argc > 1 && argv[1][0] == '-' && argv[1][1] == 'i' && argv[1][2] == '\0') {
        ci = 1;
        if (argc > 2) pattern = argv[2];
        if (argc > 3) file_arg = argv[3];
    }

    if (!pattern || !pattern[0]) {
        put_line("Usage: grep <pattern> [file] [CI]");
        return 5;
    }

    if (!file_arg || !file_arg[0]) {
        put_line("Usage: grep <pattern> [file] [CI]");
        return 5;
    }

    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(file_arg, path, sizeof(path));

    long fd = uaos_open(path, UAOS_O_RDONLY);
    if (fd < 0) { put_s("Cannot open: "); put_line(path); return 5; }

    struct uaos_stat st;
    uint32_t sz = 0;
    if (uaos_stat(path, &st) == 0) sz = st.size;

    int nl = (int)uaos_strlen(pattern);
    uint32_t pos = 0;
    int hits = 0;

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
        if (grep_match(pattern, nl, buf, col, ci)) {
            put_line((char *)buf);
            hits++;
        }
    }

    uaos_close((int)fd);

    char summary[UAOS_CMD_LINE_MAX];
    summary[0] = '\0';
    uint_to_dec((uint32_t)hits, summary, sizeof(summary));
    uaos_strlcat(summary, hits == 1 ? " match" : " matches", sizeof(summary));
    put_line(summary);
    return 0;
}
