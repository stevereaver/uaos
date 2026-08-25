/* printf.c — GNU coreutils 'printf' for UAOS gnu: layer
 *
 * Format and print data.
 *   printf FORMAT [ARGUMENT]...
 * Supports: %s, %d, %i, %x, %o, %c, %%, %u, %f (basic), \n, \t, \\, \0NNN
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static void print_escapes(const char *s)
{
    while (*s) {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
                case 'n': put_c('\n'); break;
                case 't': put_c('\t'); break;
                case 'r': put_c('\r'); break;
                case '\\': put_c('\\'); break;
                case 'a': put_c('\a'); break;
                case 'b': put_c('\b'); break;
                case 'f': put_c('\f'); break;
                case 'v': put_c('\v'); break;
                case '0':
                    s++;
                    if (*s >= '0' && *s <= '7') {
                        int v = 0;
                        for (int i = 0; i < 3 && *s >= '0' && *s <= '7'; i++) {
                            v = v * 8 + (*s - '0'); s++;
                        }
                        s--;
                        put_c((char)v);
                    } else { s--; put_c('\0'); }
                    break;
                default: put_c('\\'); put_c(*s); break;
            }
        } else {
            put_c(*s);
        }
        s++;
    }
}

static long parse_int(const char *s)
{
    long v = 0; int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

int main(int argc, const char **argv)
{
    /* printf doesn't use getopt — first operand is the format */
    if (argc < 2) { put_line("printf: usage: printf format [arguments]..."); return 1; }

    const char *fmt = argv[1];
    const char **args = (const char **)&argv[2];
    int nargs = argc - 2;
    int arg_idx = 0;

    /* If format is reused for remaining args, loop */
    int do_loop = 1;
    while (do_loop) {
        do_loop = 0;
        const char *p = fmt;
        while (*p) {
            if (*p == '\\' && p[1]) {
                /* escape — handle inline */
                char esc[2] = {'\\', p[1]};
                print_escapes(esc);
                p += 2;
                continue;
            }
            if (*p == '%') {
                p++;
                if (*p == '%') { put_c('%'); p++; continue; }
                if (*p == '\0') { put_c('%'); break; }

                /* parse flags and width (simplified) */
                int width = 0, precision = -1;
                int zero_pad = 0, left_just = 0;
                while (*p == '-' || *p == '0') {
                    if (*p == '-') left_just = 1;
                    if (*p == '0') zero_pad = 1;
                    p++;
                }
                while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
                if (*p == '.') {
                    p++;
                    precision = 0;
                    while (*p >= '0' && *p <= '9') { precision = precision * 10 + (*p - '0'); p++; }
                }

                char spec = *p;
                if (spec == '\0') break;
                p++;

                const char *arg = (arg_idx < nargs) ? args[arg_idx++] : "";
                char buf[32];

                switch (spec) {
                    case 's': {
                        int slen = (int)uaos_strlen(arg);
                        if (precision >= 0 && slen > precision) slen = precision;
                        if (!left_just && width > slen) {
                            for (int i = slen; i < width; i++) put_c(' ');
                        }
                        uaos_write(1, arg, slen);
                        if (left_just && width > slen) {
                            for (int i = slen; i < width; i++) put_c(' ');
                        }
                        break;
                    }
                    case 'd': case 'i': {
                        long v = parse_int(arg);
                        int_to_dec((int32_t)v, buf, sizeof(buf));
                        int slen = (int)uaos_strlen(buf);
                        if (!left_just && width > slen) {
                            for (int i = slen; i < width; i++) put_c(zero_pad ? '0' : ' ');
                        }
                        put_s(buf);
                        break;
                    }
                    case 'u': {
                        long v = parse_int(arg);
                        uint_to_dec((uint32_t)v, buf, sizeof(buf));
                        int slen = (int)uaos_strlen(buf);
                        if (!left_just && width > slen) {
                            for (int i = slen; i < width; i++) put_c(zero_pad ? '0' : ' ');
                        }
                        put_s(buf);
                        break;
                    }
                    case 'x': {
                        long v = parse_int(arg);
                        static const char *h = "0123456789abcdef";
                        char hex[16]; int hi = 0;
                        if (v == 0) { hex[hi++] = '0'; }
                        while (v > 0 && hi < 15) { hex[hi++] = h[v & 0xF]; v >>= 4; }
                        hex[hi] = '\0';
                        /* reverse */
                        for (int i = 0; i < hi / 2; i++) { char t = hex[i]; hex[i] = hex[hi-1-i]; hex[hi-1-i] = t; }
                        int slen = hi;
                        if (!left_just && width > slen) {
                            for (int i = slen; i < width; i++) put_c(zero_pad ? '0' : ' ');
                        }
                        put_s(hex);
                        break;
                    }
                    case 'o': {
                        long v = parse_int(arg);
                        char oct[16]; int oi = 0;
                        if (v == 0) { oct[oi++] = '0'; }
                        while (v > 0 && oi < 15) { oct[oi++] = '0' + (v & 7); v >>= 3; }
                        oct[oi] = '\0';
                        for (int i = 0; i < oi / 2; i++) { char t = oct[i]; oct[i] = oct[oi-1-i]; oct[oi-1-i] = t; }
                        int slen = oi;
                        if (!left_just && width > slen) {
                            for (int i = slen; i < width; i++) put_c(zero_pad ? '0' : ' ');
                        }
                        put_s(oct);
                        break;
                    }
                    case 'c': {
                        if (arg[0]) put_c(arg[0]);
                        break;
                    }
                    case 'f': {
                        /* simplified: just print the arg as-is */
                        put_s(arg);
                        break;
                    }
                    case 'b': {
                        /* %b interprets backslash escapes in the arg */
                        print_escapes(arg);
                        break;
                    }
                    default:
                        put_c('%'); put_c(spec);
                        arg_idx--;
                        break;
                }
            } else {
                /* check for escape sequences in format */
                if (*p == '\\') {
                    char esc[2] = {'\\', 0};
                    if (p[1]) { esc[1] = p[1]; print_escapes(esc); p += 2; }
                    else { put_c(*p); p++; }
                } else {
                    put_c(*p);
                    p++;
                }
            }
        }
        /* if there are remaining args and format had a % spec, loop */
        if (arg_idx < nargs) do_loop = 1;
    }
    return 0;
}
