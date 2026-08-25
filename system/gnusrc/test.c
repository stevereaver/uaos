/* test.c — GNU coreutils 'test' / '[' for UAOS gnu: layer
 *
 * Evaluate expression.
 *   test EXPRESSION
 *   [ EXPRESSION ]
 * Options: -e FILE, -f FILE, -d FILE, -r FILE, -w FILE, -x FILE,
 *          -s FILE, -z STRING, -n STRING, STRING1 = STRING2, STRING1 != STRING2,
 *          INT1 -eq INT2, -ne, -lt, -le, -gt, -ge, ! EXPR, -a, -o
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int is_bracket = 0; /* called as '[' */

static int test_file(const char *path, char op)
{
    char apath[UAOS_CMD_PATH_MAX];
    cmd_make_abs(path, apath, sizeof(apath));
    struct uaos_stat st;
    int exists = (uaos_stat(apath, &st) == 0);
    switch (op) {
        case 'e': return exists;
        case 'f': return exists && !st.is_dir;
        case 'd': return exists && st.is_dir;
        case 'r': return exists && !(st.protection & UAOS_FIBF_READ);
        case 'w': return exists && !(st.protection & UAOS_FIBF_WRITE);
        case 'x': return exists && !(st.protection & UAOS_FIBF_EXECUTE);
        case 's': return exists && st.size > 0;
        default: return 0;
    }
}

static long parse_int(const char *s)
{
    long v = 0; int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

static int test_expr(int argc, const char **argv, int start, int end)
{
    /* very simplified: handle single and double operand expressions */
    int nargs = end - start;
    if (nargs == 0) return 1; /* false */
    if (nargs == 1) return argv[start][0] != '\0'; /* -n STRING */

    if (nargs == 2) {
        if (argv[start][0] == '!') return !test_expr(argc, argv, start + 1, end);
        /* unary file test: -X FILE */
        if (argv[start][0] == '-' && argv[start][2] == '\0') {
            return test_file(argv[start + 1], argv[start][1]);
        }
        /* -z STRING, -n STRING */
        if (uaos_strcmp(argv[start], "-z") == 0) return argv[start + 1][0] == '\0';
        if (uaos_strcmp(argv[start], "-n") == 0) return argv[start + 1][0] != '\0';
    }

    if (nargs == 3) {
        const char *a = argv[start], *op = argv[start + 1], *b = argv[start + 2];
        if (uaos_strcmp(op, "=") == 0 || uaos_strcmp(op, "==") == 0)
            return uaos_strcmp(a, b) == 0;
        if (uaos_strcmp(op, "!=") == 0)
            return uaos_strcmp(a, b) != 0;
        if (uaos_strcmp(op, "-eq") == 0) return parse_int(a) == parse_int(b);
        if (uaos_strcmp(op, "-ne") == 0) return parse_int(a) != parse_int(b);
        if (uaos_strcmp(op, "-lt") == 0) return parse_int(a) < parse_int(b);
        if (uaos_strcmp(op, "-le") == 0) return parse_int(a) <= parse_int(b);
        if (uaos_strcmp(op, "-gt") == 0) return parse_int(a) > parse_int(b);
        if (uaos_strcmp(op, "-ge") == 0) return parse_int(a) >= parse_int(b);
        if (uaos_strcmp(op, "-a") == 0)
            return test_expr(argc, argv, start, start + 1) && test_expr(argc, argv, start + 2, end);
        if (uaos_strcmp(op, "-o") == 0)
            return test_expr(argc, argv, start, start + 1) || test_expr(argc, argv, start + 2, end);
    }

    /* handle ! with more args */
    if (nargs >= 2 && argv[start][0] == '!' && argv[start][1] == '\0') {
        return !test_expr(argc, argv, start + 1, end);
    }

    /* handle -a / -o chains (left to right) */
    for (int i = start + 1; i < end - 1; i++) {
        if (uaos_strcmp(argv[i], "-o") == 0) {
            return test_expr(argc, argv, start, i) || test_expr(argc, argv, i + 1, end);
        }
    }
    for (int i = start + 1; i < end - 1; i++) {
        if (uaos_strcmp(argv[i], "-a") == 0) {
            return test_expr(argc, argv, start, i) && test_expr(argc, argv, i + 1, end);
        }
    }

    return 1; /* default: false */
}

int main(int argc, const char **argv)
{
    int start = 1;
    int end = argc;

    /* check if called as '[' */
    if (argc > 0 && argv[0][0] == '[') {
        is_bracket = 1;
        /* last arg should be ']' */
        if (argc > 1 && uaos_strcmp(argv[argc - 1], "]") == 0) {
            end = argc - 1;
        } else {
            put_line("test: missing ']'");
            return 2;
        }
    }

    int result = test_expr(argc, argv, start, end);
    return result ? 0 : 1;
}
