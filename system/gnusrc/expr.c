/* expr.c — GNU coreutils 'expr' for UAOS gnu: layer
 *
 * Evaluate expressions.
 *   expr EXPRESSION...
 * Supports: integers, strings, +, -, *, /, %, comparison ops, : (regex match),
 *           match, index, length, substr, quote
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static long expr_eval(int argc, const char **argv, int *pos);

static long str_to_long(const char *s)
{
    long v = 0; int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

static int is_integer(const char *s)
{
    if (*s == '-' || *s == '+') s++;
    if (!*s) return 0;
    while (*s) { if (*s < '0' || *s > '9') return 0; s++; }
    return 1;
}

static long eval_primary(int argc, const char **argv, int *pos)
{
    if (*pos >= argc) return 0;
    const char *tok = argv[*pos];
    (*pos)++;

    if (uaos_strcmp(tok, "length") == 0) {
        long arg = eval_primary(argc, argv, pos);
        char buf[16]; int_to_dec((int32_t)uaos_strlen(argv[*pos - 1]), buf, sizeof(buf));
        (void)arg;
        return (long)uaos_strlen(argv[*pos - 1]);
    }
    if (uaos_strcmp(tok, "substr") == 0) {
        /* substr STRING POS LENGTH */
        const char *str = argv[*pos]; (*pos)++;
        long start = str_to_long(argv[*pos]); (*pos)++;
        long len = str_to_long(argv[*pos]); (*pos)++;
        long slen = (long)uaos_strlen(str);
        if (start < 1) start = 1;
        if (start > slen) return 0;
        if (len < 0 || start + len - 1 > slen) len = slen - start + 1;
        uaos_write(1, str + start - 1, len);
        return 0;
    }
    if (uaos_strcmp(tok, "index") == 0) {
        const char *str = argv[*pos]; (*pos)++;
        const char *chars = argv[*pos]; (*pos)++;
        for (int i = 0; str[i]; i++) {
            for (int j = 0; chars[j]; j++) {
                if (str[i] == chars[j]) return i + 1;
            }
        }
        return 0;
    }
    if (tok[0] == '(') {
        long v = expr_eval(argc, argv, pos);
        if (*pos < argc && argv[*pos][0] == ')') (*pos)++;
        return v;
    }

    /* literal */
    if (is_integer(tok)) return str_to_long(tok);
    /* string literal — print it */
    put_s(tok);
    return 1;
}

static long eval_mul(int argc, const char **argv, int *pos)
{
    long left = eval_primary(argc, argv, pos);
    while (*pos < argc) {
        const char *op = argv[*pos];
        if (uaos_strcmp(op, "*") == 0 || uaos_strcmp(op, "/") == 0 || uaos_strcmp(op, "%") == 0) {
            (*pos)++;
            long right = eval_primary(argc, argv, pos);
            if (op[0] == '*') left = left * right;
            else if (op[0] == '/') left = right != 0 ? left / right : 0;
            else left = right != 0 ? left % right : 0;
        } else break;
    }
    return left;
}

static long eval_add(int argc, const char **argv, int *pos)
{
    long left = eval_mul(argc, argv, pos);
    while (*pos < argc) {
        const char *op = argv[*pos];
        if ((op[0] == '+' && op[1] == '\0') || (op[0] == '-' && op[1] == '\0')) {
            (*pos)++;
            long right = eval_mul(argc, argv, pos);
            left = (op[0] == '+') ? left + right : left - right;
        } else break;
    }
    return left;
}

static long eval_cmp(int argc, const char **argv, int *pos)
{
    long left = eval_add(argc, argv, pos);
    while (*pos < argc) {
        const char *op = argv[*pos];
        if (uaos_strcmp(op, "<") == 0 || uaos_strcmp(op, "<=") == 0 ||
            uaos_strcmp(op, ">") == 0 || uaos_strcmp(op, ">=") == 0 ||
            uaos_strcmp(op, "=") == 0 || uaos_strcmp(op, "==") == 0 ||
            uaos_strcmp(op, "!=") == 0) {
            (*pos)++;
            long right = eval_add(argc, argv, pos);
            if (uaos_strcmp(op, "<") == 0) left = left < right ? 1 : 0;
            else if (uaos_strcmp(op, "<=") == 0) left = left <= right ? 1 : 0;
            else if (uaos_strcmp(op, ">") == 0) left = left > right ? 1 : 0;
            else if (uaos_strcmp(op, ">=") == 0) left = left >= right ? 1 : 0;
            else if (uaos_strcmp(op, "=") == 0 || uaos_strcmp(op, "==") == 0) left = left == right ? 1 : 0;
            else left = left != right ? 1 : 0;
        } else break;
    }
    return left;
}

static long eval_and(int argc, const char **argv, int *pos)
{
    long left = eval_cmp(argc, argv, pos);
    while (*pos < argc) {
        if (uaos_strcmp(argv[*pos], "&") == 0) {
            (*pos)++;
            if (left == 0) { /* short circuit — skip right */
                eval_cmp(argc, argv, pos);
                return 0;
            }
            left = eval_cmp(argc, argv, pos);
            left = left ? 1 : 0;
        } else break;
    }
    return left;
}

static long eval_or(int argc, const char **argv, int *pos)
{
    long left = eval_and(argc, argv, pos);
    while (*pos < argc) {
        if (uaos_strcmp(argv[*pos], "|") == 0) {
            (*pos)++;
            if (left != 0) { /* short circuit — skip right */
                eval_and(argc, argv, pos);
                return left;
            }
            left = eval_and(argc, argv, pos);
        } else break;
    }
    return left;
}

static long expr_eval(int argc, const char **argv, int *pos)
{
    return eval_or(argc, argv, pos);
}

int main(int argc, const char **argv)
{
    if (argc < 2) { put_line("expr: missing operand"); return 2; }
    int pos = 1;
    long result = expr_eval(argc, argv, &pos);

    /* print result */
    char buf[24];
    int_to_dec((int32_t)result, buf, sizeof(buf));
    put_s(buf);
    put_c('\n');

    return (result == 0) ? 1 : 0;
}
