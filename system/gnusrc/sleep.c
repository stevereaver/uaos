/* sleep.c — GNU coreutils 'sleep' for UAOS gnu: layer
 *
 * Delay for a specified amount of time.
 *   sleep NUMBER[SUFFIX]...
 * NUMBER may be fractional (sleep 0.5); suffixes: s (seconds, default),
 * m (minutes), h (hours), d (days).
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = { {NULL, 0, 0} };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "", long_opts, &li)) != -1) {
        return 1;
    }

    int nops = uaos_operands_count(argc);
    if (nops == 0) { put_line("sleep: missing operand"); return 1; }

    /* Sum all durations in milliseconds so fractional seconds work. */
    uint64_t total_ms = 0;
    for (int i = 0; i < nops; i++) {
        const char *arg = uaos_operand(argc, argv, i);
        if (!arg) continue;
        const char *p = arg;
        long val = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; digits++; }
        long frac_ms = 0;
        if (*p == '.') {
            p++;
            int scale = 100;
            while (*p >= '0' && *p <= '9' && scale) {
                frac_ms += (*p - '0') * scale;
                scale /= 10;
                p++;
                digits++;
            }
            while (*p >= '0' && *p <= '9') p++; /* extra digits ignored */
        }
        if (!digits) {
            put_s("sleep: invalid time interval '");
            put_s(arg);
            put_line("'");
            return 1;
        }
        long mult = 1;
        if (*p == 's') mult = 1;
        else if (*p == 'm') mult = 60;
        else if (*p == 'h') mult = 3600;
        else if (*p == 'd') mult = 86400;
        total_ms += (uint64_t)val * (uint64_t)mult * 1000ULL +
                    (uint64_t)frac_ms * (uint64_t)mult;
    }

    uaos_sleep_ms((long)total_ms);
    return 0;
}
