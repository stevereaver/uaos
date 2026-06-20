/* find.c — UAOS x86-64 userspace 'find' command
 *
 * Recursively walks a directory tree and prints paths, with optional filters
 * by name (-name glob) and type (-type f|d).  Uses the INT 0x80 syscall
 * interface; no standard library is required.
 */

#include "uaos_syscall.h"
#include "uaos_libc.h"

#define PATH_MAX 256

struct options {
    const char *start_path;
    const char *name_pattern;
    char        type_filter;   /* 'f' or 'd', 0 = none */
};

static void put_s(const char *s)
{
    uaos_write(1, s, (long)uaos_strlen(s));
}

static void put_c(char c)
{
    uaos_write(1, &c, 1);
}

static void join_path(char *dst, const char *base, const char *name, size_t max)
{
    size_t i = 0;
    while (i < max - 1 && base[i]) {
        dst[i] = base[i];
        i++;
    }
    if (i > 0 && dst[i - 1] != ':' && dst[i - 1] != '/' && i + 1 < max)
        dst[i++] = '/';
    size_t j = 0;
    while (i + 1 < max && name[j]) {
        dst[i] = name[j];
        i++;
        j++;
    }
    dst[i] = '\0';
}

static int fnmatch(const char *pat, const char *s)
{
    for (;;) {
        if (*pat == '\0')
            return *s == '\0';
        if (*pat == '*') {
            pat++;
            if (*pat == '\0')
                return 1;
            while (*s) {
                if (fnmatch(pat, s))
                    return 1;
                s++;
            }
            return *pat == '\0';
        }
        if (*pat == '?') {
            if (*s == '\0')
                return 0;
            pat++;
            s++;
            continue;
        }
        if (*pat != *s)
            return 0;
        pat++;
        s++;
    }
}

static int match_entry(const char *name, int is_dir, const struct options *opt)
{
    if (opt->name_pattern && !fnmatch(opt->name_pattern, name))
        return 0;
    if (opt->type_filter) {
        if (opt->type_filter == 'f' && is_dir)
            return 0;
        if (opt->type_filter == 'd' && !is_dir)
            return 0;
    }
    return 1;
}

static void print_path(const char *path)
{
    put_s(path);
    put_c('\n');
}

static int walk(const char *path, const struct options *opt)
{
    long dd = uaos_opendir(path);
    if (dd < 0) {
        put_s("find: ");
        put_s(path);
        put_s(": cannot open directory\n");
        return 1;
    }

    struct uaos_dirent ent;
    while (uaos_readdir((int)dd, &ent) > 0) {
        if (ent.name[0] == '\0')
            continue;
        /* Skip '.' and '..' analogues if they ever appear. */
        if (ent.name[0] == '.' &&
            (ent.name[1] == '\0' || (ent.name[1] == '.' && ent.name[2] == '\0')))
            continue;

        char full[PATH_MAX];
        join_path(full, path, ent.name, sizeof(full));

        if (match_entry(ent.name, ent.is_dir, opt))
            print_path(full);

        if (ent.is_dir)
            walk(full, opt);
    }

    uaos_closedir((int)dd);
    return 0;
}

static int parse_options(int argc, const char **argv, struct options *opt)
{
    opt->start_path = NULL;
    opt->name_pattern = NULL;
    opt->type_filter = 0;

    int i = 1;
    while (i < argc) {
        const char *arg = argv[i];
        if (arg[0] == '-' && arg[1] == 'n' && arg[2] == 'a' &&
            arg[3] == 'm' && arg[4] == 'e' && arg[5] == '\0') {
            if (i + 1 >= argc)
                return -1;
            opt->name_pattern = argv[i + 1];
            i += 2;
        } else if (arg[0] == '-' && arg[1] == 't' && arg[2] == 'y' &&
                   arg[3] == 'p' && arg[4] == 'e' && arg[5] == '\0') {
            if (i + 1 >= argc)
                return -1;
            opt->type_filter = argv[i + 1][0];
            i += 2;
        } else if (arg[0] == '-') {
            return -1;
        } else {
            break;
        }
    }

    if (i < argc)
        opt->start_path = argv[i];

    return 0;
}

int main(int argc, const char **argv)
{
    struct options opt;
    if (parse_options(argc, argv, &opt) != 0) {
        put_s("usage: find [path] [-name pattern] [-type f|d]\n");
        return 1;
    }

    char cwd[128];
    if (!opt.start_path) {
        long n = uaos_getcwd(cwd, sizeof(cwd));
        if (n <= 0)
            cwd[0] = '\0';
        opt.start_path = cwd;
    }

    if (opt.start_path[0] == '\0') {
        put_s("find: no starting path\n");
        return 1;
    }

    return walk(opt.start_path, &opt);
}
