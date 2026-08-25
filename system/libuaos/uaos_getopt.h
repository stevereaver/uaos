/* uaos_getopt.h — minimal GNU-style getopt-long for freestanding UAOS programs
 *
 * Header-only implementation of a subset of POSIX getopt / GNU getopt_long
 * for the UAOS `gnu:` POSIX compatibility layer.  No standard library is
 * required — each utility is a single translation unit so the static state
 * below is safe.
 *
 * Supported syntax:
 *   -x            short option, no argument
 *   -xyz          combined short options (-x -y -z)
 *   -n5           short option with argument attached
 *   -n 5          short option with argument in next argv slot
 *   --long        long option, no argument
 *   --long=val    long option with argument attached
 *   --long val    long option with argument in next argv slot (required only)
 *   --            end of option parsing; remaining argv are operands
 *
 * has_arg values:
 *   no_argument       0  — option takes no argument
 *   required_argument 1  — option requires an argument
 *   optional_argument 2  — argument optional, only accepted attached (--opt=val)
 *
 * Return value of uaos_getopt_long():
 *   The short option character, or UAOS_GO_LONG (0x100 + index) for a long
 *   option, or -1 when all options have been consumed.  On unknown option
 *   returns '?'.  g_optarg is set to the option argument (or NULL).
 *   After the loop, g_optind is the index of the first operand.
 */

#ifndef UAOS_GETOPT_H
#define UAOS_GETOPT_H

#include <stdint.h>
#include <stddef.h>
#include "uaos_libc.h"

#define no_argument        0
#define required_argument  1
#define optional_argument  2

#define UAOS_GO_LONG 0x100  /* base for long-option return values */

typedef struct {
    const char *long_name;   /* long name without "--", or NULL */
    char        short_name;  /* short option char, or 0 */
    int         has_arg;     /* no_argument / required_argument / optional_argument */
} uaos_long_opt_t;

/* Global state (single translation unit per utility — safe) */
static const char *g_optarg = NULL;
static int   g_optind = 1;
static int   g_optopt = 0;
static int   g_opterr = 1;
static const char *g_short_cursor = NULL;  /* cursor within a combined short opt */

static int uaos_getopt_long(int argc, const char **argv,
                            const char *short_opts,
                            const uaos_long_opt_t *long_opts,
                            int *long_idx)
{
    g_optarg = NULL;
    if (long_idx) *long_idx = -1;

    /* If we are mid-way through a combined short option string, continue. */
    if (g_short_cursor && *g_short_cursor) {
        const char *sopts = short_opts;
        char c = *g_short_cursor++;
        g_optopt = (int)(unsigned char)c;
        const char *p = sopts;
        while (*p && *p != c) p++;
        if (!*p) {
            if (g_opterr) { /* unknown short option */ }
            return '?';
        }
        /* Check for required/optional argument in short_opts spec */
        if (p[1] == ':') {
            if (*g_short_cursor) {
                /* -n5 : rest of this slot is the argument */
                g_optarg = g_short_cursor;
                g_short_cursor = NULL;
            } else if (p[2] == ':') {
                /* optional argument — only accepted attached, none here */
            } else if (g_optind < argc) {
                /* -n 5 : next argv slot */
                g_optarg = argv[g_optind++];
            } else {
                /* missing required argument */
                return ':';
            }
        }
        return (int)(unsigned char)c;
    }
    g_short_cursor = NULL;

    if (g_optind >= argc)
        return -1;

    const char *arg = argv[g_optind];
    if (!arg || arg[0] != '-' || arg[1] == '\0') {
        /* Operand (or "-" which means stdin) — stop parsing. */
        return -1;
    }

    if (arg[1] == '-' && arg[2] == '\0') {
        /* "--" ends option parsing. */
        g_optind++;
        return -1;
    }

    if (arg[1] == '-') {
        /* Long option: --name or --name=value */
        const char *name = arg + 2;
        const char *eq = name;
        while (*eq && *eq != '=') eq++;
        int name_len = (int)(eq - name);
        const char *attached = (*eq == '=') ? eq + 1 : NULL;

        if (long_opts) {
            for (int i = 0; long_opts[i].long_name; i++) {
                if ((int)uaos_strlen(long_opts[i].long_name) == name_len &&
                    uaos_strncmp(long_opts[i].long_name, name, name_len) == 0) {
                    g_optind++;
                    if (long_idx) *long_idx = i;
                    if (long_opts[i].has_arg == required_argument) {
                        if (attached) {
                            g_optarg = attached;
                        } else if (g_optind < argc) {
                            g_optarg = argv[g_optind++];
                        } else {
                            return ':';
                        }
                    } else if (long_opts[i].has_arg == optional_argument) {
                        if (attached) g_optarg = attached;
                    }
                    if (long_opts[i].short_name)
                        return (int)(unsigned char)long_opts[i].short_name;
                    return UAOS_GO_LONG + i;
                }
            }
        }
        /* Unknown long option */
        g_optind++;
        g_optopt = '?';
        return '?';
    }

    /* Short option(s): -x, -xyz, -n5 */
    g_optind++;
    g_short_cursor = arg + 1;
    /* Recurse to process the first short option in this slot. */
    return uaos_getopt_long(argc, argv, short_opts, long_opts, long_idx);
}

/* Reset getopt state (useful if a program parses twice). */
static inline void uaos_getopt_reset(void)
{
    g_optarg = NULL;
    g_optind = 1;
    g_optopt = 0;
    g_short_cursor = NULL;
}

/* -------------------------------------------------------------------------
 * Operand helpers — after option parsing, argv[g_optind..argc-1] are
 * operands.  These helpers collect them.
 * ------------------------------------------------------------------------- */

/* Count operands remaining after g_optind. */
static inline int uaos_operands_count(int argc)
{
    return argc - g_optind;
}

/* Get operand at relative index (0-based from g_optind). */
static inline const char *uaos_operand(int argc, const char **argv, int idx)
{
    int abs = g_optind + idx;
    if (abs >= argc) return NULL;
    return argv[abs];
}

/* -------------------------------------------------------------------------
 * Numeric helpers for option arguments
 * ------------------------------------------------------------------------- */

/* Parse g_optarg as a long.  Returns 1 on success, 0 on failure. */
static inline int uaos_optarg_long(long *out)
{
    if (!g_optarg) return 0;
    const char *p = g_optarg;
    int neg = 0;
    long val = 0;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') { p++; }
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    if (*p != '\0') return 0;
    *out = neg ? -val : val;
    return 1;
}

#endif /* UAOS_GETOPT_H */
