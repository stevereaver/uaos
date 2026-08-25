/* numfmt.c — GNU coreutils 'numfmt' for UAOS gnu: layer
 *
 * Format numbers.
 *   numfmt [OPTION]... [NUMBER]...
 * Options: --from=UNIT, --to=UNIT, --suffix=STRING, --padding=N,
 *          --field=N, --header[=N], --round=METHOD, --delimiter=CHAR
 * Units: none, auto, si, iec, iec-i  (e.g. 1K=1000, 1Ki=1024)
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static char opt_from[8] = "none";
static char opt_to[8] = "none";
static char opt_suffix[16] = "";
static int  opt_padding = 0;
static int  opt_field = 1;
static char opt_delim = '\n';

static long parse_number(const char *s)
{
    long val = 0;
    int neg = 0;
    const char *p = s;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') { p++; }
    while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
    /* handle suffix */
    if (*p) {
        char c = *p;
        long mult = 1;
        if (c == 'K' || c == 'k') mult = (uaos_strcmp(opt_from, "iec") == 0) ? 1024 : 1000;
        else if (c == 'M' || c == 'm') mult = (uaos_strcmp(opt_from, "iec") == 0) ? 1048576 : 1000000;
        else if (c == 'G' || c == 'g') mult = (uaos_strcmp(opt_from, "iec") == 0) ? 1073741824L : 1000000000L;
        val *= mult;
    }
    return neg ? -val : val;
}

static void format_number(long val)
{
    long abs_val = val < 0 ? -val : val;
    char buf[32];

    if (uaos_strcmp(opt_to, "si") == 0 || uaos_strcmp(opt_to, "iec") == 0) {
        int is_iec = (uaos_strcmp(opt_to, "iec") == 0);
        long divisor = is_iec ? 1024 : 1000;
        const char *suffixes = is_iec ? "KMGTPE" : "KMGTPE";
        int si = 0;
        long v = abs_val;
        while (v >= divisor && si < 6) { v /= divisor; si++; }
        int_to_dec(v, buf, sizeof(buf));
        int slen = (int)uaos_strlen(buf);
        /* padding */
        if (opt_padding > 0) {
            for (int i = slen; i < opt_padding; i++) put_c(' ');
        }
        if (val < 0) put_c('-');
        put_s(buf);
        if (si > 0) { put_c(suffixes[si - 1]); if (is_iec) put_c('i'); }
    } else {
        int_to_dec(val, buf, sizeof(buf));
        int slen = (int)uaos_strlen(buf);
        if (opt_padding > 0) {
            for (int i = slen; i < opt_padding; i++) put_c(' ');
        }
        put_s(buf);
    }
    if (opt_suffix[0]) put_s(opt_suffix);
    put_c('\n');
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"from",     0, required_argument},
        {"to",       0, required_argument},
        {"suffix",   0, required_argument},
        {"padding",  0, required_argument},
        {"field",    0, required_argument},
        {"delimiter",0, required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "", long_opts, &li)) != -1) {
        const char *arg = g_optarg;
        switch (opt) {
            case UAOS_GO_LONG + 0: if (arg) { int i=0; while (arg[i] && i<(int)sizeof(opt_from)-1) { opt_from[i]=arg[i]; i++; } opt_from[i]='\0'; } break;
            case UAOS_GO_LONG + 1: if (arg) { int i=0; while (arg[i] && i<(int)sizeof(opt_to)-1) { opt_to[i]=arg[i]; i++; } opt_to[i]='\0'; } break;
            case UAOS_GO_LONG + 2: if (arg) { int i=0; while (arg[i] && i<(int)sizeof(opt_suffix)-1) { opt_suffix[i]=arg[i]; i++; } opt_suffix[i]='\0'; } break;
            case UAOS_GO_LONG + 3: if (arg) { long v; if (uaos_optarg_long(&v)) opt_padding = (int)v; } break;
            case UAOS_GO_LONG + 4: if (arg) { long v; if (uaos_optarg_long(&v)) opt_field = (int)v; } break;
            case UAOS_GO_LONG + 5: if (arg) opt_delim = arg[0]; break;
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    if (nops == 0) {
        /* read from stdin */
        char numbuf[32]; int col = 0;
        for (;;) {
            uint8_t ch; long n = uaos_read(0, &ch, 1);
            if (n <= 0) break;
            if (ch == '\n' || ch == opt_delim) {
                if (col > 0) { numbuf[col] = '\0'; format_number(parse_number(numbuf)); col = 0; }
            } else if (col < (int)sizeof(numbuf) - 1) {
                numbuf[col++] = (char)ch;
            }
        }
        if (col > 0) { numbuf[col] = '\0'; format_number(parse_number(numbuf)); }
        return 0;
    }
    for (int i = 0; i < nops; i++) {
        const char *arg = uaos_operand(argc, argv, i);
        if (arg) format_number(parse_number(arg));
    }
    return 0;
}
