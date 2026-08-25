/* sleep.c — GNU coreutils 'sleep' for UAOS gnu: layer
 *
 * Delay for a specified amount of time.
 *   sleep NUMBER[SUFFIX]...
 * Suffixes: s (seconds, default), m (minutes), h (hours), d (days)
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

    /* sum all durations */
    long total_ticks = 0;
    for (int i = 0; i < nops; i++) {
        const char *arg = uaos_operand(argc, argv, i);
        if (!arg) continue;
        long val = 0;
        const char *p = arg;
        while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
        long mult = 1;
        if (*p == 's') mult = 1;
        else if (*p == 'm') mult = 60;
        else if (*p == 'h') mult = 3600;
        else if (*p == 'd') mult = 86400;
        total_ticks += val * mult;
    }

    /* UAOS uses the schedule syscall to yield.  We yield repeatedly.
     * The kernel tick rate is roughly 100 Hz, so 100 yields ≈ 1 second. */
    for (long i = 0; i < total_ticks * 100; i++) {
        uaos_yield();
    }
    return 0;
}
