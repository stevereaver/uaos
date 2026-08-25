/* factor.c — GNU coreutils 'factor' for UAOS gnu: layer
 *
 * Print prime factors of numbers.
 *   factor [NUMBER]...
 * Options: --exponents
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static long str_to_long(const char *s)
{
    long v = 0; int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

static void factor_number(uint64_t n)
{
    char buf[24];
    uint_to_dec((uint32_t)n, buf, sizeof(buf));
    put_s(buf); put_c(':');

    if (n <= 1) { put_c('\n'); return; }

    /* factor out 2s */
    while (n % 2 == 0) {
        put_s(" 2");
        n /= 2;
    }

    /* factor out odd numbers */
    for (uint64_t i = 3; i * i <= n; i += 2) {
        while (n % i == 0) {
            put_s(" ");
            uint_to_dec((uint32_t)i, buf, sizeof(buf));
            put_s(buf);
            n /= i;
        }
    }

    if (n > 1) {
        put_s(" ");
        uint_to_dec((uint32_t)n, buf, sizeof(buf));
        put_s(buf);
    }
    put_c('\n');
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"exponents", 0, no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "", long_opts, &li)) != -1) {
        switch (opt) {
            default: break; /* ignore --exponents for now */
        }
    }

    int nops = uaos_operands_count(argc);
    if (nops == 0) {
        /* read from stdin */
        char numbuf[24]; int col = 0;
        for (;;) {
            uint8_t ch; long n = uaos_read(0, &ch, 1);
            if (n <= 0) break;
            if (uaos_isspace(ch)) {
                if (col > 0) { numbuf[col] = '\0'; factor_number((uint64_t)str_to_long(numbuf)); col = 0; }
            } else if (col < (int)sizeof(numbuf) - 1) {
                numbuf[col++] = (char)ch;
            }
        }
        if (col > 0) { numbuf[col] = '\0'; factor_number((uint64_t)str_to_long(numbuf)); }
        return 0;
    }

    for (int i = 0; i < nops; i++) {
        const char *arg = uaos_operand(argc, argv, i);
        if (!arg) continue;
        /* parse number */
        uint64_t v = 0;
        const char *p = arg;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
        factor_number(v);
    }
    return 0;
}
