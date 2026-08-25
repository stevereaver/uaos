/* echo.c — UAOS x86-64 userspace 'echo' command
 *
 * AmigaDOS C:Echo — print text to the shell.
 *   echo <string> [NOLINE] [FIRST <n>] [LEN <n>] [TO <file>]
 *
 * Template: STRING/M,NOLINE/S,FIRST/K/N,LEN/K/N,TO/K
 */

#include "uaos_cmd.h"
#include "uaos_template.h"

int main(int argc, const char **argv)
{
    char args[UAOS_TMPL_MAX_VAL];
    cmd_build_args(argc, argv, args, sizeof(args));

    UaosTmpl t;
    uaos_tmpl_run("STRING/M,NOLINE/S,FIRST/K/N,LEN/K/N,TO/K", &t, args);
    if (t.error[0]) { put_s("echo: "); put_line(t.error); return 20; }

    /* Concatenate all STRING values. */
    char text[UAOS_TMPL_MAX_VAL];
    text[0] = '\0';
    int n = uaos_tmpl_count(&t, "STRING");
    for (int i = 0; i < n; i++) {
        const char *v = uaos_tmpl_multi(&t, "STRING", i);
        if (!v) continue;
        if (i > 0) uaos_strlcat(text, " ", sizeof(text));
        uaos_strlcat(text, v, sizeof(text));
    }

    int noline = uaos_tmpl_switch(&t, "NOLINE");
    int first = 0, len = -1;
    uaos_tmpl_int(&t, "FIRST", &first);
    int got_len = uaos_tmpl_int(&t, "LEN", &len);
    if (!got_len) len = -1;

    /* Apply FIRST (start offset) and LEN (character count). */
    int slen = (int)uaos_strlen(text);
    if (first < 0) first = 0;
    if (first > slen) first = slen;
    if (len < 0) len = slen - first;
    if (first + len > slen) len = slen - first;

    /* Determine output destination. */
    long out_fd = 1; /* stdout */
    const char *to_file = uaos_tmpl_string(&t, "TO");
    if (to_file && to_file[0]) {
        char abs_to[UAOS_CMD_PATH_MAX];
        cmd_make_abs(to_file, abs_to, sizeof(abs_to));
        out_fd = uaos_open(abs_to, UAOS_O_WRONLY | UAOS_O_CREAT | UAOS_O_TRUNC);
        if (out_fd < 0) { put_s("echo: cannot open: "); put_line(to_file); return 5; }
    }

    if (len > 0) {
        if (out_fd == 1) {
            uaos_write(1, text + first, len);
        } else {
            uaos_write_file((int)out_fd, (const uint8_t *)(text + first), (uint32_t)len);
        }
    }
    if (!noline) {
        if (out_fd == 1) put_c('\n');
        else uaos_write_file((int)out_fd, (const uint8_t *)"\n", 1);
    }

    if (out_fd != 1) uaos_close((int)out_fd);
    return 0;
}
