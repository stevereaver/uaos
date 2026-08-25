/* more.c — UAOS x86-64 userspace 'more' command
 *
 * AmigaDOS C:More — paginate file output one screen at a time.
 *   more <file>
 *
 * After each page a "--More--" prompt is shown and the command waits for:
 *   Space / Page-Down : advance one full page
 *   Enter             : advance one line
 *   q / Escape        : quit immediately
 */

#include "uaos_cmd.h"
#include "uaos_template.h"

int main(int argc, const char **argv)
{
    char args[UAOS_TMPL_MAX_VAL];
    cmd_build_args(argc, argv, args, sizeof(args));

    UaosTmpl t;
    uaos_tmpl_run("FILE", &t, args);
    if (t.error[0]) { put_s("more: "); put_line(t.error); return 20; }

    const char *file_arg = uaos_tmpl_string(&t, "FILE");
    if (!file_arg || !file_arg[0]) {
        put_line("Usage: more <file>");
        return 5;
    }

    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(file_arg, path, sizeof(path));

    long fd = uaos_open(path, UAOS_O_RDONLY);
    if (fd < 0) { put_s("Cannot open: "); put_line(path); return 5; }

    struct uaos_stat st;
    uint32_t sz = 0;
    if (uaos_stat(path, &st) == 0) sz = st.size;

    /* Page height: default 20 lines (visible-rows is not exposed to
     * userspace tasks; the shell renders output line-by-line anyway). */
    int page_h = 20;
    uint32_t pos = 0;
    int line_in_page = 0;

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
        put_line((char *)buf);
        line_in_page++;

        if (line_in_page >= page_h && pos < sz) {
            put_line("--More-- [Space=page  Enter=line  q=quit]");
            long k = uaos_readkey();
            put_line("");
            if (k == 'q' || k == 'Q' || k == 27) {
                uaos_close((int)fd);
                return 0;
            } else if (k == '\r' || k == '\n') {
                line_in_page = page_h - 1;
            } else {
                line_in_page = 0;
            }
        }
    }
    uaos_close((int)fd);
    return 0;
}
