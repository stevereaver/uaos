/* seq.c — GNU coreutils 'seq' for UAOS gnu: layer
 *
 * Print a sequence of numbers.
 *   seq [OPTION]... LAST
 *   seq [OPTION]... FIRST LAST
 *   seq [OPTION]... FIRST INCREMENT LAST
 * Options: -f FORMAT, --format=FORMAT, -s STRING, --separator=STRING,
 *          -w, --equal-width
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_equal_width = 0;
static char opt_sep[16] = "\n";
static char opt_fmt[64] = "";

static void print_num(long val, int width, long scale)
{
    char buf[32];
    if (scale > 1) {
        /* fixed-point: val is scaled by 'scale' */
        long ip = val / scale;
        long fp = val % scale;
        if (fp < 0) { fp = -fp; ip--; }
        int_to_dec(ip, buf, sizeof(buf));
        put_s(buf);
        put_c('.');
        /* print fractional part with leading zeros */
        char fbuf[12];
        int_to_dec(fp, fbuf, sizeof(fbuf));
        int flen = (int)uaos_strlen(fbuf);
        int digits = 0;
        long s = scale;
        while (s > 1) { s /= 10; digits++; }
        for (int i = flen; i < digits; i++) put_c('0');
        put_s(fbuf);
    } else {
        int_to_dec(val, buf, sizeof(buf));
        int slen = (int)uaos_strlen(buf);
        if (opt_equal_width && width > slen) {
            for (int i = 0; i < width - slen; i++) put_c('0');
        }
        put_s(buf);
    }
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"format",     'f', required_argument},
        {"separator",  's', required_argument},
        {"equal-width",'w', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "f:s:w", long_opts, &li)) != -1) {
        switch (opt) {
            case 'f': if (g_optarg) { int i=0; while (g_optarg[i] && i < (int)sizeof(opt_fmt)-1) { opt_fmt[i]=g_optarg[i]; i++; } opt_fmt[i]='\0'; } break;
            case 's': if (g_optarg) { int i=0; while (g_optarg[i] && i < (int)sizeof(opt_sep)-1) { opt_sep[i]=g_optarg[i]; i++; } opt_sep[i]='\0'; } break;
            case 'w': opt_equal_width = 1; break;
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    if (nops < 1 || nops > 3) { put_line("seq: usage: seq LAST or seq FIRST LAST or seq FIRST INC LAST"); return 1; }

    long first = 1, inc = 1, last = 1;
    /* parse operands manually since they're plain numbers */
    const char *a0 = uaos_operand(argc, argv, 0);
    const char *a1 = uaos_operand(argc, argv, 1);
    const char *a2 = uaos_operand(argc, argv, 2);

    /* simple integer parse */
    long vals[3] = {0,0,0};
    const char *strs[3] = {a0, a1, a2};
    for (int i = 0; i < nops; i++) {
        const char *p = strs[i];
        int neg = 0; long v = 0;
        if (*p == '-') { neg = 1; p++; }
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
        vals[i] = neg ? -v : v;
    }
    if (nops == 1) { first = 1; inc = 1; last = vals[0]; }
    else if (nops == 2) { first = vals[0]; inc = 1; last = vals[1]; }
    else { first = vals[0]; inc = vals[1]; last = vals[2]; }

    if (inc == 0) { put_line("seq: increment cannot be 0"); return 1; }

    int width = 0;
    if (opt_equal_width) {
        char buf[32];
        int_to_dec(first > last ? first : last, buf, sizeof(buf));
        width = (int)uaos_strlen(buf);
        if (first < 0 || last < 0) width++;
    }

    int first_out = 1;
    if (inc > 0) {
        for (long v = first; v <= last; v += inc) {
            if (!first_out) put_s(opt_sep);
            print_num(v, width, 1);
            first_out = 0;
        }
    } else {
        for (long v = first; v >= last; v += inc) {
            if (!first_out) put_s(opt_sep);
            print_num(v, width, 1);
            first_out = 0;
        }
    }
    if (!first_out) put_c('\n');
    return 0;
}
