/* join.c — UAOS x86-64 userspace 'join' command
 *
 * AmigaDOS C:Join — concatenate multiple files into a destination file.
 *   join <file1> [file2] ... AS <dest>
 *
 * Template: FILE/M/A,AS=TO/K/A
 */

#include "uaos_cmd.h"
#include "uaos_template.h"

static void dump_file(long out_fd, const char *name)
{
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(name, path, sizeof(path));

    long fd = uaos_open(path, UAOS_O_RDONLY);
    if (fd < 0) {
        put_s("Cannot open: ");
        put_line(path);
        return;
    }

    struct uaos_stat st;
    uint32_t sz = 0;
    if (uaos_stat(path, &st) == 0) sz = st.size;

    uint32_t pos = 0;
    while (pos < sz) {
        char buf[256];
        long n = uaos_read_file((int)fd, (uint8_t *)buf, sizeof(buf));
        if (n <= 0) break;
        uaos_write_file((int)out_fd, (const uint8_t *)buf, (uint32_t)n);
        pos += (uint32_t)n;
    }
    uaos_close((int)fd);
}

int main(int argc, const char **argv)
{
    char args[UAOS_TMPL_MAX_VAL];
    cmd_build_args(argc, argv, args, sizeof(args));

    UaosTmpl t;
    uaos_tmpl_run("FILE/M/A,AS=TO/K/A", &t, args);
    if (t.error[0]) { put_s("join: "); put_line(t.error); return 20; }

    const char *to_arg = uaos_tmpl_string(&t, "TO");
    if (!to_arg || !to_arg[0]) {
        put_line("Usage: join <file1> [file2] ... AS <dest>");
        return 5;
    }

    int n = uaos_tmpl_count(&t, "FILE");
    if (n == 0) {
        put_line("Usage: join <file1> [file2] ... AS <dest>");
        return 5;
    }

    char to_path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(to_arg, to_path, sizeof(to_path));
    long out_fd = uaos_open(to_path, UAOS_O_WRONLY | UAOS_O_CREAT | UAOS_O_TRUNC);
    if (out_fd < 0) { put_s("Cannot create: "); put_line(to_path); return 5; }

    for (int i = 0; i < n; i++) {
        const char *file = uaos_tmpl_multi(&t, "FILE", i);
        if (file && file[0]) dump_file(out_fd, file);
    }

    uaos_close((int)out_fd);
    put_s("Joined ");
    char num[8];
    uint_to_dec((uint32_t)n, num, sizeof(num));
    put_s(num);
    put_s(" files into ");
    put_line(to_arg);
    return 0;
}
