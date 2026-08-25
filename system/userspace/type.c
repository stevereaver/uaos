/* type.c — UAOS x86-64 userspace 'type' command
 *
 * AmigaDOS C:Type — print file contents to the shell.
 *   type <from> [TO <file>] [OPT H|N] [HEX] [NUMBER]
 *
 * Template: FROM/A/M,TO/K,OPT/K,HEX/S,NUMBER/S
 */

#include "uaos_cmd.h"
#include "uaos_template.h"

static void type_hex(long fd, uint32_t sz)
{
    uint8_t buf[16];
    uint32_t pos = 0;
    while (pos < sz) {
        long n = uaos_read_file((int)fd, buf, (sz - pos < 16) ? (sz - pos) : 16);
        if (n <= 0) break;
        char line[UAOS_CMD_LINE_MAX];
        line[0] = '\0';
        char num[12];
        uint_to_dec(pos, num, 12);
        uaos_strlcat(line, num, sizeof(line));
        uaos_strlcat(line, ": ", sizeof(line));
        for (int i = 0; i < n; i++) {
            char hex[4];
            hex[0] = "0123456789ABCDEF"[buf[i] >> 4];
            hex[1] = "0123456789ABCDEF"[buf[i] & 0xF];
            hex[2] = ' ';
            hex[3] = '\0';
            uaos_strlcat(line, hex, sizeof(line));
        }
        for (int i = n; i < 16; i++) uaos_strlcat(line, "   ", sizeof(line));
        uaos_strlcat(line, " ", sizeof(line));
        for (int i = 0; i < n; i++) {
            char c = (buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.';
            int li = (int)uaos_strlen(line);
            if (li < (int)sizeof(line) - 1) { line[li] = c; line[li + 1] = '\0'; }
        }
        put_line(line);
        pos += (uint32_t)n;
    }
}

static void type_text(long fd, uint32_t sz, int numbers, long out_fd)
{
    char buf[UAOS_CMD_LINE_MAX];
    uint32_t pos = 0;
    int line_no = 0;
    while (pos < sz) {
        int col = 0;
        while (pos < sz && col < (int)sizeof(buf) - 1) {
            uint8_t c;
            if (uaos_read_file((int)fd, &c, 1) == 0) break;
            pos++;
            if (c == '\n') break;
            if (c != '\r') buf[col++] = (char)c;
        }
        buf[col] = '\0';
        line_no++;
        char line[UAOS_CMD_LINE_MAX];
        line[0] = '\0';
        if (numbers) {
            char lnum[8];
            uint_to_dec((uint32_t)line_no, lnum, 8);
            uaos_strlcat(line, lnum, sizeof(line));
            uaos_strlcat(line, ": ", sizeof(line));
        }
        uaos_strlcat(line, buf, sizeof(line));
        if (out_fd >= 0) {
            uaos_write_file((int)out_fd, (const uint8_t *)line, (uint32_t)uaos_strlen(line));
            uaos_write_file((int)out_fd, (const uint8_t *)"\n", 1);
        } else {
            put_line(line);
        }
    }
}

static void type_one(const char *file_arg, int hex, int numbers, long out_fd)
{
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(file_arg, path, sizeof(path));

    long fd = uaos_open(path, UAOS_O_RDONLY);
    if (fd < 0) { put_s("Cannot open: "); put_line(path); return; }

    struct uaos_stat st;
    uint32_t sz = 0;
    if (uaos_stat(path, &st) == 0) sz = st.size;

    if (hex) type_hex(fd, sz);
    else     type_text(fd, sz, numbers, out_fd);

    uaos_close((int)fd);
}

int main(int argc, const char **argv)
{
    char args[UAOS_TMPL_MAX_VAL];
    cmd_build_args(argc, argv, args, sizeof(args));

    UaosTmpl t;
    uaos_tmpl_run("FROM/A/M,TO/K,OPT/K,HEX/S,NUMBER/S", &t, args);
    if (t.error[0]) { put_s("type: "); put_line(t.error); return 20; }

    int hex = uaos_tmpl_switch(&t, "HEX");
    int numbers = uaos_tmpl_switch(&t, "NUMBER");

    /* OPT keyword: H=hex, N=number */
    const char *opt = uaos_tmpl_string(&t, "OPT");
    if (opt) {
        for (int i = 0; opt[i]; i++) {
            char c = opt[i]; if (c >= 'A' && c <= 'Z') c += 32;
            if (c == 'h') hex = 1;
            if (c == 'n') numbers = 1;
        }
    }

    long out_fd = -1;
    const char *to_file = uaos_tmpl_string(&t, "TO");
    if (to_file) {
        char abs_to[UAOS_CMD_PATH_MAX];
        cmd_make_abs(to_file, abs_to, sizeof(abs_to));
        out_fd = uaos_open(abs_to, UAOS_O_WRONLY | UAOS_O_CREAT | UAOS_O_TRUNC);
        if (out_fd < 0) { put_s("Cannot create: "); put_line(abs_to); }
    }

    int n = uaos_tmpl_count(&t, "FROM");
    if (n == 0) {
        put_line("Usage: type <from> [TO <file>] [OPT H|N] [HEX] [NUMBER]");
        return 5;
    }
    for (int i = 0; i < n; i++) {
        const char *file_arg = uaos_tmpl_multi(&t, "FROM", i);
        if (file_arg) type_one(file_arg, hex, numbers, out_fd);
    }

    if (out_fd >= 0) uaos_close((int)out_fd);
    return 0;
}
