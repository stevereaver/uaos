/* strings.c — UAOS x86-64 userspace 'strings' command
 *
 * Scans files for printable character sequences of a minimum length and
 * prints them, similar to the Unix strings utility.  Uses the INT 0x80
 * syscall interface; no standard library is required.
 */

#include "uaos_syscall.h"
#include "uaos_libc.h"

#define BUF_SIZE   4096
#define STR_MAX    256

static int is_printable(char c)
{
    unsigned char uc = (unsigned char)c;
    return uaos_isprint(uc) || uc == ' ' || uc == '\t';
}

static void put_s(const char *s)
{
    uaos_write(1, s, (long)uaos_strlen(s));
}

static void put_c(char c)
{
    uaos_write(1, &c, 1);
}

static int parse_min_len(const char *arg)
{
    long n = 0;
    while (uaos_isdigit(*arg)) {
        n = n * 10 + (*arg - '0');
        arg++;
    }
    if (n <= 0)
        return 4;
    return (int)n;
}

static int strings_file(const char *path, int min_len, int show_name,
                        const char *name)
{
    long fd = uaos_open(path, UAOS_O_RDONLY);
    if (fd < 0) {
        put_s("strings: ");
        put_s(path);
        put_s(": cannot open\n");
        return 1;
    }

    char run[STR_MAX + 1];
    int run_len = 0;
    char buf[BUF_SIZE];
    long n;

    while ((n = uaos_read_file((int)fd, buf, BUF_SIZE)) > 0) {
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (is_printable(c)) {
                if (run_len < STR_MAX)
                    run[run_len++] = c;
            } else {
                if (run_len >= min_len) {
                    run[run_len] = '\0';
                    if (show_name) {
                        put_s(name);
                        put_s(": ");
                    }
                    put_s(run);
                    put_c('\n');
                }
                run_len = 0;
            }
        }
    }

    /* Flush a trailing run. */
    if (run_len >= min_len) {
        run[run_len] = '\0';
        if (show_name) {
            put_s(name);
            put_s(": ");
        }
        put_s(run);
        put_c('\n');
    }

    uaos_close((int)fd);
    return 0;
}

int main(int argc, const char **argv)
{
    int min_len = 4;
    int first_file = 1;

    if (argc > 1 && uaos_strcmp(argv[1], "-n") == 0) {
        if (argc < 3) {
            put_s("usage: strings [-n minlen] file...\n");
            return 1;
        }
        min_len = parse_min_len(argv[2]);
        first_file = 3;
    } else if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'n' &&
               argv[1][2] == '=') {
        min_len = parse_min_len(argv[1] + 3);
        first_file = 2;
    }

    if (first_file >= argc) {
        put_s("usage: strings [-n minlen] file...\n");
        return 1;
    }

    int show_name = (argc - first_file) > 1;
    int rc = 0;

    for (int i = first_file; i < argc; i++) {
        const char *path = argv[i];
        const char *name = path;
        const char *p = path;
        while (*p) {
            if (*p == '/' || *p == ':')
                name = p + 1;
            p++;
        }
        if (strings_file(path, min_len, show_name, name) != 0)
            rc = 1;
    }

    return rc;
}
